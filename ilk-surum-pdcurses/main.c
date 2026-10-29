#define _CRT_SECURE_NO_WARNINGS
#define PDC_STATIC_BUILD

#include <locale.h>
#include <curses.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>

#define MAX_LINES 1000
#define MAX_COLS 1000
#define MAX_FILES 500
#define MAX_UNDO 100
#define MAX_MATCHES 500
#define CLIPBOARD 4096

#define CTRL_RIGHT 560
#define CTRL_LEFT 561
#define CTRL_UP 562
#define CTRL_DOWN 563
#define CTRL_SHIFT_RIGHT 564
#define CTRL_SHIFT_LEFT 565
#define CTRL_SHIFT_UP 566
#define CTRL_SHIFT_DOWN 567

char lines[MAX_LINES][MAX_COLS];
int line_count = 1;
int cursor_row = 0;
int cursor_col = 0;
int scroll_row = 0;
char current_file[512] = "";

// ========== UNDO ==========
typedef struct {
    char lines[MAX_LINES][MAX_COLS];
    int line_count, cursor_row, cursor_col;
} UndoState;

UndoState undo_stack[MAX_UNDO];
UndoState redo_stack[MAX_UNDO];
int redo_top = 0;
int undo_top = 0;

void undo_push() {
    redo_top = 0;
    if (undo_top >= MAX_UNDO) {
        int i;
        for (i = 0; i < MAX_UNDO - 1; i++) undo_stack[i] = undo_stack[i + 1];
        undo_top = MAX_UNDO - 1;
    }
    memcpy(undo_stack[undo_top].lines, lines, sizeof(lines));
    undo_stack[undo_top].line_count = line_count;
    undo_stack[undo_top].cursor_row = cursor_row;
    undo_stack[undo_top].cursor_col = cursor_col;
    undo_top++;
}

void undo_pop() {
    if (undo_top <= 0) return;
    memcpy(redo_stack[redo_top].lines, lines, sizeof(lines));
    redo_stack[redo_top].line_count = line_count;
    redo_stack[redo_top].cursor_row = cursor_row;
    redo_stack[redo_top].cursor_col = cursor_col;
    redo_top++;
    undo_top--;
    memcpy(lines, undo_stack[undo_top].lines, sizeof(lines));
    line_count = undo_stack[undo_top].line_count;
    cursor_row = undo_stack[undo_top].cursor_row;
    cursor_col = undo_stack[undo_top].cursor_col;
}

void redo_pop() {
    if (redo_top <= 0) return;
    redo_top--;
    memcpy(lines, redo_stack[redo_top].lines, sizeof(lines));
    line_count = redo_stack[redo_top].line_count;
    cursor_row = redo_stack[redo_top].cursor_row;
    cursor_col = redo_stack[redo_top].cursor_col;
}

// ========== SEÇÝM ==========
int sel_active = 0;
int sel_start_row = 0, sel_start_col = 0;
int sel_end_row = 0, sel_end_col = 0;
char clipboard[CLIPBOARD] = "";

int pos_cmp(int r1, int c1, int r2, int c2) {
    if (r1 < r2) return -1;
    if (r1 > r2) return 1;
    if (c1 < c2) return -1;
    if (c1 > c2) return 1;
    return 0;
}

void sel_ordered(int* sr, int* sc, int* er, int* ec) {
    if (pos_cmp(sel_start_row, sel_start_col, sel_end_row, sel_end_col) <= 0) {
        *sr = sel_start_row; *sc = sel_start_col;
        *er = sel_end_row; *ec = sel_end_col;
    }
    else {
        *sr = sel_end_row; *sc = sel_end_col;
        *er = sel_start_row; *ec = sel_start_col;
    }
}

int is_selected(int row, int col) {
    if (!sel_active) return 0;
    int sr, sc, er, ec;
    sel_ordered(&sr, &sc, &er, &ec);
    return pos_cmp(sr, sc, row, col) <= 0 && pos_cmp(row, col, er, ec) < 0;
}

void copy_selection() {
    if (!sel_active) return;
    int sr, sc, er, ec;
    sel_ordered(&sr, &sc, &er, &ec);
    clipboard[0] = '\0';
    int i;
    for (i = sr; i <= er; i++) {
        int from = (i == sr) ? sc : 0;
        int to = (i == er) ? ec : (int)strlen(lines[i]);
        strncat(clipboard, lines[i] + from, to - from);
        if (i < er) strcat(clipboard, "\n");
    }
}

void delete_selection() {
    if (!sel_active) return;
    undo_push();
    int sr, sc, er, ec;
    sel_ordered(&sr, &sc, &er, &ec);
    char tail[MAX_COLS];
    strncpy(tail, lines[er] + ec, MAX_COLS - 1);
    tail[MAX_COLS - 1] = '\0';
    lines[sr][sc] = '\0';
    strncat(lines[sr], tail, MAX_COLS - 1 - sc);
    int del = er - sr;
    int i;
    for (i = 0; i < del; i++) {
        int idx = sr + 1;
        if (idx < line_count) {
            int j;
            for (j = idx; j < line_count - 1; j++) strcpy(lines[j], lines[j + 1]);
            memset(lines[line_count - 1], 0, MAX_COLS);
            line_count--;
        }
    }
    cursor_row = sr;
    cursor_col = sc;
    sel_active = 0;
}

void paste_clipboard() {
    if (strlen(clipboard) == 0) return;
    undo_push();
    char* p = clipboard;
    while (*p) {
        if (*p == '\n') {
            if (line_count < MAX_LINES - 1) {
                int i;
                for (i = line_count; i > cursor_row + 1; i--) strcpy(lines[i], lines[i - 1]);
                strcpy(lines[cursor_row + 1], lines[cursor_row] + cursor_col);
                lines[cursor_row][cursor_col] = '\0';
                line_count++;
                cursor_row++;
                cursor_col = 0;
            }
        }
        else {
            int len = strlen(lines[cursor_row]);
            if (len < MAX_COLS - 1) {
                memmove(&lines[cursor_row][cursor_col + 1], &lines[cursor_row][cursor_col], len - cursor_col + 1);
                lines[cursor_row][cursor_col] = *p;
                cursor_col++;
            }
        }
        p++;
    }
}

// ========== ARAMA ==========
typedef struct { int row, col; } Match;
Match matches[MAX_MATCHES];
int match_count = 0, current_match = -1;
char search_term[256] = "";

void find_matches(const char* term) {
    match_count = 0;
    current_match = -1;
    if (!strlen(term)) return;
    int i;
    for (i = 0; i < line_count; i++) {
        char* ptr = lines[i];
        while ((ptr = strstr(ptr, term)) != NULL) {
            if (match_count < MAX_MATCHES) {
                matches[match_count].row = i;
                matches[match_count].col = ptr - lines[i];
                match_count++;
            }
            ptr++;
        }
    }
    if (match_count > 0) current_match = 0;
}

void replace_all(const char* old_term, const char* new_term) {
    int old_len = strlen(old_term), new_len = strlen(new_term);
    int i;
    for (i = 0; i < line_count; i++) {
        char temp[MAX_COLS];
        char* src = lines[i], * dst = temp;
        while (*src) {
            if (strncmp(src, old_term, old_len) == 0) {
                strncpy(dst, new_term, new_len);
                dst += new_len; src += old_len;
            }
            else { *dst++ = *src++; }
        }
        *dst = '\0';
        strcpy(lines[i], temp);
    }
}

// ========== KARAKTER ÝÞLEMLERÝ ==========
void insert_char(int row, int col, char c) {
    int len = strlen(lines[row]);
    if (len >= MAX_COLS - 1) return;
    memmove(&lines[row][col + 1], &lines[row][col], len - col + 1);
    lines[row][col] = c;
}

void delete_char(int row, int col) {
    int len = strlen(lines[row]);
    if (col >= len) return;
    memmove(&lines[row][col], &lines[row][col + 1], len - col);
}

void insert_newline(int row, int col) {
    if (line_count >= MAX_LINES - 1) return;
    int i;
    for (i = line_count; i > row + 1; i--) strcpy(lines[i], lines[i - 1]);
    strcpy(lines[row + 1], &lines[row][col]);
    lines[row][col] = '\0';
    line_count++;
}

void delete_line(int row) {
    if (line_count <= 1) return;
    int i;
    for (i = row; i < line_count - 1; i++) strcpy(lines[i], lines[i + 1]);
    memset(lines[line_count - 1], 0, MAX_COLS);
    line_count--;
}

void merge_with_above(int row) {
    if (row <= 0) return;
    int above_len = strlen(lines[row - 1]);
    strcat(lines[row - 1], lines[row]);
    delete_line(row);
    cursor_row--;
    cursor_col = above_len;
}

void delete_word_before() {
    if (cursor_col == 0) {
        merge_with_above(cursor_row);
        return;
    }
    int col = cursor_col;
    while (col > 0 && lines[cursor_row][col - 1] == ' ') col--;
    while (col > 0 && lines[cursor_row][col - 1] != ' ') col--;
    memmove(&lines[cursor_row][col], &lines[cursor_row][cursor_col], strlen(lines[cursor_row]) - cursor_col + 1);
    cursor_col = col;
}

// ========== DOSYA ÝÞLEMLERÝ ==========
void save_file(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) return;
    int i;
    for (i = 0; i < line_count; i++) fprintf(f, "%s\n", lines[i]);
    fclose(f);
    strncpy(current_file, filename, 511);
}

void load_file(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) return;
    memset(lines, 0, sizeof(lines));
    line_count = 0;
    while (fgets(lines[line_count], MAX_COLS, f)) {
        int len = strlen(lines[line_count]);
        if (len > 0 && lines[line_count][len - 1] == '\n') lines[line_count][len - 1] = '\0';
        line_count++;
    }
    fclose(f);
    if (line_count == 0) line_count = 1;
    strncpy(current_file, filename, 511);
    cursor_row = 0; cursor_col = 0; scroll_row = 0;
}

// ========== DOSYA YÖNETÝCÝSÝ ==========
typedef struct { char name[256]; int is_dir; } FileEntry;
FileEntry file_list[MAX_FILES];
int file_count = 0;

void list_directory(const char* path) {
    WIN32_FIND_DATAA findFileData;
    HANDLE hFind = INVALID_HANDLE_VALUE;
    char search_path[512];

    snprintf(search_path, sizeof(search_path), "%s\\*", path);

    file_count = 0;
    strcpy(file_list[file_count].name, "..");
    file_list[file_count].is_dir = 1;
    file_count++;

    hFind = FindFirstFileA(search_path, &findFileData);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(findFileData.cFileName, ".") != 0 && strcmp(findFileData.cFileName, "..") != 0) {
            if (file_count < MAX_FILES) {
                strcpy(file_list[file_count].name, findFileData.cFileName);
                file_list[file_count].is_dir = (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
                file_count++;
            }
        }
    } while (FindNextFileA(hFind, &findFileData) != 0);

    FindClose(hFind);
}

void file_manager(char* result, int mod) {
    char cwd[512];
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    int selected = 0, scroll = 0;
    while (1) {
        list_directory(cwd);
        clear();
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        attron(A_REVERSE);
        mvprintw(0, 0, mod == 0 ? " DOSYA AC - %s " : " DOSYA KAYDET - %s ", cwd);
        attroff(A_REVERSE);
        int visible = rows - 4, i;
        for (i = 0; i < visible && (i + scroll) < file_count; i++) {
            int idx = i + scroll;
            if (idx == selected) attron(A_REVERSE);
            mvprintw(i + 2, 2, file_list[idx].is_dir ? "[KLASOR] %s" : "         %s", file_list[idx].name);
            if (idx == selected) attroff(A_REVERSE);
        }
        attron(A_REVERSE);
        mvprintw(rows - 1, 0, mod == 0 ? " Enter:Ac  Backspace:Ust Dizin  ESC:Iptal " : " Enter:Sec  N:Yeni Dosya  Backspace:Ust Dizin ESC:Iptal ");
        attroff(A_REVERSE);
        refresh();
        int ch = getch();
        if (ch == 27) { result[0] = '\0'; return; }
        else if (ch == KEY_UP) {
            if (selected > 0) selected--;
            if (selected < scroll) scroll--;
        }
        else if (ch == KEY_DOWN) {
            if (selected < file_count - 1) selected++;
            if (selected >= scroll + visible) scroll++;
        }
        else if (ch == '\n') {
            if (file_list[selected].is_dir) {
                if (!strcmp(file_list[selected].name, "..")) {
                    char* l = strrchr(cwd, '\\');
                    if (l && l != cwd) *l = '\0';
                }
                else {
                    char np[512];
                    snprintf(np, sizeof(np), "%s\\%s", cwd, file_list[selected].name);
                    strncpy(cwd, np, 511);
                }
                selected = 0; scroll = 0;
            }
            else {
                snprintf(result, 511, "%s\\%s", cwd, file_list[selected].name);
                return;
            }
        }
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            char* l = strrchr(cwd, '\\');
            if (l && l != cwd) *l = '\0';
            selected = 0; scroll = 0;
        }
        else if (mod == 1 && (ch == 'n' || ch == 'N')) {
            int r2, c2;
            getmaxyx(stdscr, r2, c2);
            char fname[256] = "";
            int i2 = 0, ch2;
            attron(A_REVERSE);
            mvprintw(r2 - 2, 0, " Yeni dosya adi: %-30s", "");
            attroff(A_REVERSE);
            refresh();
            while ((ch2 = getch()) != '\n' && ch2 != 27) {
                if ((ch2 == 127 || ch2 == KEY_BACKSPACE || ch2 == 8) && i2 > 0) {
                    i2--; fname[i2] = '\0';
                }
                else if (ch2 >= 32 && ch2 < 127) {
                    fname[i2++] = ch2; fname[i2] = '\0';
                }
                attron(A_REVERSE);
                mvprintw(r2 - 2, 0, " Yeni dosya adi: %-30s", fname);
                attroff(A_REVERSE);
                refresh();
            }
            if (strlen(fname) > 0) { snprintf(result, 511, "%s\\%s", cwd, fname); return; }
        }
    }
}

// ========== EKRAN ÇÝZÝMÝ ==========
void draw_toolbox() {
    attron(A_REVERSE);
    mvprintw(0, 0, " Ac(^O) Kaydet(^S) FarkliKaydet(^A) Ara(^F) Sonraki(^G) Degistir(^R) GeriAl(^Z) Kopyala(^C) Kes(^X) Yapistir(^V) Cikis(ESC) ");
    attroff(A_REVERSE);
}

void draw_editor() {
    int i;
    int term_len = strlen(search_term);
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int visible = rows - 3;
    for (i = 0; i < visible; i++) {
        int line_idx = i + scroll_row;
        if (line_idx >= line_count) break;
        mvprintw(i + 2, 0, "%3d | ", line_idx + 1);
        if (term_len == 0 && !sel_active) {
            mvprintw(i + 2, 6, "%s", lines[line_idx]);
        }
        else {
            int j = 0;
            int len = strlen(lines[line_idx]);
            while (j < len) {
                int sel = is_selected(line_idx, j);
                int found = (!sel && term_len > 0 && strncmp(&lines[line_idx][j], search_term, term_len) == 0);
                if (found) {
                    attron(COLOR_PAIR(1));
                    mvprintw(i + 2, 6 + j, "%.*s", term_len, &lines[line_idx][j]);
                    attroff(COLOR_PAIR(1));
                    j += term_len;
                }
                else if (sel) {
                    attron(COLOR_PAIR(2));
                    mvprintw(i + 2, 6 + j, "%c", lines[line_idx][j]);
                    attroff(COLOR_PAIR(2));
                    j++;
                }
                else {
                    mvprintw(i + 2, 6 + j, "%c", lines[line_idx][j]);
                    j++;
                }
            }
        }
    }
}

void draw_statusbar() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    attron(A_REVERSE);
    mvprintw(rows - 1, 0, " Satir:%d Sutun:%d Dosya:%s%s",
        cursor_row + 1, cursor_col + 1,
        strlen(current_file) > 0 ? current_file : "[Yeni Dosya]",
        match_count > 0 ? " " : "");
    if (match_count > 0) mvprintw(rows - 1, 40, "Eslesen:%d/%d ", current_match + 1, match_count);
    attroff(A_REVERSE);
}

// ========== PROMPT FONKSIYONLARI ==========
void search_prompt() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    char term[256] = "";
    int i = 0, ch;
    attron(A_REVERSE);
    mvprintw(rows - 2, 0, " Ara: %-40s", "");
    attroff(A_REVERSE);
    refresh();
    while ((ch = getch()) != '\n' && ch != 27) {
        if ((ch == 127 || ch == KEY_BACKSPACE || ch == 8) && i > 0) {
            i--; term[i] = '\0';
        }
        else if (ch >= 32 && ch < 127) {
            term[i++] = ch; term[i] = '\0';
        }
        attron(A_REVERSE);
        mvprintw(rows - 2, 0, " Ara: %-40s", term);
        attroff(A_REVERSE);
        refresh();
    }
    if (ch == 27) { search_term[0] = '\0'; match_count = 0; return; }
    strncpy(search_term, term, 255);
    find_matches(search_term);
    if (match_count > 0) { cursor_row = matches[0].row; cursor_col = matches[0].col; }
}

void replace_prompt() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    char old_term[256] = "", new_term[256] = "";
    int i = 0, ch;
    attron(A_REVERSE);
    mvprintw(rows - 3, 0, " Degistirilecek: %-30s", "");
    attroff(A_REVERSE);
    refresh();
    while ((ch = getch()) != '\n' && ch != 27) {
        if ((ch == 127 || ch == KEY_BACKSPACE || ch == 8) && i > 0) {
            i--; old_term[i] = '\0';
        }
        else if (ch >= 32 && ch < 127) {
            old_term[i++] = ch; old_term[i] = '\0';
        }
        attron(A_REVERSE);
        mvprintw(rows - 3, 0, " Degistirilecek: %-30s", old_term);
        attroff(A_REVERSE);
        refresh();
    }
    if (ch == 27 || !strlen(old_term)) return;
    i = 0;
    attron(A_REVERSE);
    mvprintw(rows - 2, 0, " Yeni kelime: %-30s", "");
    attroff(A_REVERSE);
    refresh();
    while ((ch = getch()) != '\n' && ch != 27) {
        if ((ch == 127 || ch == KEY_BACKSPACE || ch == 8) && i > 0) {
            i--; new_term[i] = '\0';
        }
        else if (ch >= 32 && ch < 127) {
            new_term[i++] = ch; new_term[i] = '\0';
        }
        attron(A_REVERSE);
        mvprintw(rows - 2, 0, " Yeni kelime: %-30s", new_term);
        attroff(A_REVERSE);
        refresh();
    }
    if (ch == 27) return;
    undo_push();
    replace_all(old_term, new_term);
}

// ========== TUÞU OKU ==========
int read_key() {
    int ch = getch();
    if (ch != 27) return ch;
    nodelay(stdscr, TRUE);
    int ch2 = getch();
    if (ch2 == ERR) { nodelay(stdscr, FALSE); return 27; }

    if (ch2 == '[') {
        char seq[16];
        int si = 0, c;
        while ((c = getch()) != ERR && si < 15) {
            seq[si++] = (char)c;
            if (c >= 'A' && c <= 'Z') break;
        }
        nodelay(stdscr, FALSE);
        seq[si] = '\0';
        if (strcmp(seq, "1;5C") == 0) return CTRL_RIGHT;
        if (strcmp(seq, "1;5D") == 0) return CTRL_LEFT;
        if (strcmp(seq, "1;5A") == 0) return CTRL_UP;
        if (strcmp(seq, "1;5B") == 0) return CTRL_DOWN;
        if (strcmp(seq, "1;6C") == 0) return CTRL_SHIFT_RIGHT;
        if (strcmp(seq, "1;6D") == 0) return CTRL_SHIFT_LEFT;
        if (strcmp(seq, "1;6A") == 0) return CTRL_SHIFT_UP;
        if (strcmp(seq, "1;6B") == 0) return CTRL_SHIFT_DOWN;
    }
    nodelay(stdscr, FALSE);
    return 27;
}

// ========== ANA DÖNGÜ ==========
int main() {
    SetConsoleCP(65001);
    SetConsoleOutputCP(65001);

    setlocale(LC_ALL, "tr_TR.utf8");
    setlocale(LC_ALL, "");

    initscr();
    cbreak();
    raw();

    signal(SIGINT, SIG_IGN);
    noecho();
    keypad(stdscr, TRUE);
    mousemask(0, NULL);
    curs_set(1);
    start_color();

    init_pair(1, COLOR_WHITE, COLOR_BLUE);
    init_pair(2, COLOR_BLACK, COLOR_YELLOW);

    memset(lines, 0, sizeof(lines));

    while (1) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        int visible = rows - 3;

        if (cursor_row < scroll_row) scroll_row = cursor_row;
        if (cursor_row >= scroll_row + visible) scroll_row = cursor_row - visible + 1;

        clear();
        draw_toolbox();
        draw_editor();
        draw_statusbar();
        move((cursor_row - scroll_row) + 2, cursor_col + 6);
        refresh();

        int ch = read_key();

        if (ch == 27) {
            search_term[0] = '\0';
            match_count = 0; current_match = -1; sel_active = 0;
            clear(); draw_toolbox(); draw_editor();
            attron(A_REVERSE);
            mvprintw(rows - 1, 0, " Kaydetmek istiyor musunuz? (y/n): ");
            attroff(A_REVERSE);
            refresh();
            int ans = getch();
            if (ans == 'y' || ans == 'Y') {
                if (strlen(current_file) > 0) save_file(current_file);
                else {
                    char fname[512] = "";
                    file_manager(fname, 1);
                    if (strlen(fname) > 0) save_file(fname);
                }
                break;
            }
            else if (ans == 'n' || ans == 'N') break;
        }
        else if (ch == 26) { undo_pop(); sel_active = 0; }
        else if (ch == 25) { redo_pop(); sel_active = 0; }
        else if (ch == 6) { search_prompt(); }
        else if (ch == 7) {
            if (match_count > 0) {
                current_match = (current_match + 1) % match_count;
                cursor_row = matches[current_match].row; cursor_col = matches[current_match].col;
            }
        }
        else if (ch == 23) {
            if (match_count > 0) {
                current_match = (current_match - 1 + match_count) % match_count;
                cursor_row = matches[current_match].row; cursor_col = matches[current_match].col;
            }
        }
        else if (ch == 18) { replace_prompt(); }
        else if (ch == 19) {
            if (strlen(current_file) > 0) save_file(current_file);
            else {
                char fname[512] = "";
                file_manager(fname, 1);
                if (strlen(fname) > 0) save_file(fname);
            }
        }
        else if (ch == 1) {
            char fname[512] = "";
            file_manager(fname, 1);
            if (strlen(fname) > 0) save_file(fname);
        }
        else if (ch == 15) {
            char fname[512] = "";
            file_manager(fname, 0);
            if (strlen(fname) > 0) load_file(fname);
        }
        else if (ch == 3) { copy_selection(); sel_active = 0; }
        else if (ch == 24) { copy_selection(); delete_selection(); }
        else if (ch == 22) { paste_clipboard(); }
        else if (ch == KEY_UP || ch == KEY_DOWN || ch == KEY_LEFT || ch == KEY_RIGHT || ch == 258 || ch == 259 || ch == 260 || ch == 261) {
            int ctrl_pressed = (GetKeyState(VK_CONTROL) & 0x8000);
            int shift_pressed = (GetKeyState(VK_SHIFT) & 0x8000);
            if (ctrl_pressed) {
                if (!sel_active) { sel_active = 1; sel_start_row = cursor_row; sel_start_col = cursor_col; }
            }
            else { sel_active = 0; }

            if (ch == KEY_RIGHT || ch == 261) {
                if (ctrl_pressed && shift_pressed) {
                    int len = strlen(lines[cursor_row]);
                    while (cursor_col < len && lines[cursor_row][cursor_col] == ' ') cursor_col++;
                    while (cursor_col < len && lines[cursor_row][cursor_col] != ' ') cursor_col++;
                }
                else {
                    int len = strlen(lines[cursor_row]);
                    if (cursor_col < len) cursor_col++;
                    else if (cursor_row < line_count - 1) { cursor_row++; cursor_col = 0; }
                }
            }
            else if (ch == KEY_LEFT || ch == 260) {
                if (ctrl_pressed && shift_pressed) {
                    while (cursor_col > 0 && lines[cursor_row][cursor_col - 1] == ' ') cursor_col--;
                    while (cursor_col > 0 && lines[cursor_row][cursor_col - 1] != ' ') cursor_col--;
                }
                else {
                    if (cursor_col > 0) cursor_col--;
                    else if (cursor_row > 0) { cursor_row--; cursor_col = strlen(lines[cursor_row]); }
                }
            }
            else if (ch == KEY_UP || ch == 259) {
                if (cursor_row > 0) {
                    cursor_row--; int len = strlen(lines[cursor_row]);
                    if (cursor_col > len) cursor_col = len;
                }
            }
            else if (ch == KEY_DOWN || ch == 258) {
                if (cursor_row < line_count - 1) {
                    cursor_row++; int len = strlen(lines[cursor_row]);
                    if (cursor_col > len) cursor_col = len;
                }
            }
            if (sel_active) { sel_end_row = cursor_row; sel_end_col = cursor_col; }
        }
        else if (ch == '\n') {
            sel_active = 0; undo_push();
            insert_newline(cursor_row, cursor_col);
            cursor_row++; cursor_col = 0;
        }
        else if (ch == 8) { undo_push(); delete_word_before(); }
        else if (ch == 127 || ch == KEY_BACKSPACE) {
            if (sel_active) { delete_selection(); }
            else {
                undo_push();
                if (cursor_col > 0) { cursor_col--; delete_char(cursor_row, cursor_col); }
                else { merge_with_above(cursor_row); }
            }
        }
        else if (ch >= 32) {
            if (sel_active) delete_selection();
            undo_push();
            if (ch < 128) {
                insert_char(cursor_row, cursor_col, (char)ch); cursor_col++;
            }
            else {
                insert_char(cursor_row, cursor_col, (char)ch); cursor_col++;
                nodelay(stdscr, TRUE);
                int next;
                while ((next = getch()) != ERR && next >= 128) {
                    insert_char(cursor_row, cursor_col, (char)next); cursor_col++;
                }
                if (next != ERR && next < 128) ungetch(next);
                nodelay(stdscr, FALSE);
            }
        }
    }
    endwin();
    return 0;
}