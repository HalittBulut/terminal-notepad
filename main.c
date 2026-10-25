/*
 * Mini Notepad++ - Konsol Tabanli Metin Editoru
 * Visual Studio / Windows icin - Windows Console API kullanir
 * Turkce karakter destegi: SetConsoleCP(65001) + ReadConsoleInputW
 *
 * Derleme (Visual Studio Developer Command Prompt):
 *   cl main.c /Fe:main.exe
 *
 * Calistirma:
 *   main.exe
 */

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>  /* _getcwd, _chdir */
#include <fcntl.h>
#include <io.h> // Genellikle _setmode fonksiyonu için bu da gerekir

/* Define VT mode constants for older Windows SDK versions */
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif

 /* ========== SABITLER ========== */
#define MAX_LINES     1000
#define MAX_COLS      2000   /* UTF-8 byte cinsinden max satir uzunlugu */
#define MAX_FILES     500
#define MAX_UNDO      100
#define MAX_MATCHES   500
#define CLIPBOARD_SZ  8192

/* ========== GLOBAL DURUM ========== */
static char  lines[MAX_LINES][MAX_COLS];
static int   line_count  = 1;
static int   cursor_row  = 0;
static int   cursor_col  = 0;   /* byte offset (UTF-8) */
static int   scroll_row  = 0;
static char  current_file[MAX_PATH] = "";
static int   modified    = 0;

/* Windows konsol handle'lari */
static HANDLE hIn, hOut;
static int    term_rows, term_cols;

/* ========== UTF-8 YARDIMCI FONKSIYONLARi ========== */

static int utf8_char_len(unsigned char c) {
    if ((c & 0x80) == 0)    return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}
static int utf8_is_start(unsigned char c) { return (c & 0xC0) != 0x80; }

/* Bir byte'dan baslayan UTF-8 karakteri wchar_t'ye donustur */
static wchar_t utf8_to_wchar(const char *s, int off) {
    unsigned char c = (unsigned char)s[off];
    if (c < 0x80) return (wchar_t)c;
    if ((c & 0xE0) == 0xC0)
        return (wchar_t)(((c & 0x1F) << 6) | ((unsigned char)s[off+1] & 0x3F));
    if ((c & 0xF0) == 0xE0)
        return (wchar_t)(((c & 0x0F) << 12) | (((unsigned char)s[off+1] & 0x3F) << 6)
                       | ((unsigned char)s[off+2] & 0x3F));
    return L'?';
}

/* wchar_t'yi UTF-8 byte dizisine yaz, kac byte yazildigini dondur */
static int wchar_to_utf8(wchar_t wc, char *buf) {
    if (wc < 0x80)   { buf[0] = (char)wc; return 1; }
    if (wc < 0x800)  { buf[0] = (char)(0xC0 | (wc >> 6));
                       buf[1] = (char)(0x80 | (wc & 0x3F)); return 2; }
    buf[0] = (char)(0xE0 | (wc >> 12));
    buf[1] = (char)(0x80 | ((wc >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (wc & 0x3F));
    return 3;
}

/* byte offset'ten bir sonraki karakter basina gec */
static int utf8_next(const char *s, int off) {
    int len = (int)strlen(s);
    if (off >= len) return len;
    return off + utf8_char_len((unsigned char)s[off]);
}

/* byte offset'ten bir onceki karakter basina gec */
static int utf8_prev(const char *s, int off) {
    if (off <= 0) return 0;
    off--;
    while (off > 0 && !utf8_is_start((unsigned char)s[off])) off--;
    return off;
}

/* byte offset'e kadar kac ekran sutunu var */
static int utf8_byte_to_vcol(const char *s, int off) {
    int col = 0, i = 0;
    while (i < off && s[i]) {
        if (!utf8_is_start((unsigned char)s[i])) { i++; continue; }
        i += utf8_char_len((unsigned char)s[i]);
        col++;
    }
    return col;
}

/* ekran sutunundan byte offset hesapla */
static int utf8_vcol_to_byte(const char *s, int vcol) {
    int col = 0, i = 0;
    int len = (int)strlen(s);
    while (i < len && col < vcol) {
        if (!utf8_is_start((unsigned char)s[i])) { i++; continue; }
        i += utf8_char_len((unsigned char)s[i]);
        col++;
    }
    return i;
}

/* satirdaki UTF-8 karakter sayisi (gorunur genislik) */
static int utf8_char_count(const char *s) {
    int n = 0, i = 0;
    int len = (int)strlen(s);
    while (i < len) {
        if (utf8_is_start((unsigned char)s[i])) n++;
        i++;
    }
    return n;
}

/* ========== UNDO / REDO ========== */
typedef struct {
    char lines[MAX_LINES][MAX_COLS];
    int  line_count, cursor_row, cursor_col;
} UndoState;

static UndoState undo_stack[MAX_UNDO];
static UndoState redo_stack[MAX_UNDO];
static int undo_top = 0, redo_top = 0;

static void undo_push(void) {
    redo_top = 0;
    if (undo_top >= MAX_UNDO) {
        int i;
        for (i = 0; i < MAX_UNDO - 1; i++) undo_stack[i] = undo_stack[i+1];
        undo_top = MAX_UNDO - 1;
    }
    memcpy(undo_stack[undo_top].lines, lines, sizeof(lines));
    undo_stack[undo_top].line_count  = line_count;
    undo_stack[undo_top].cursor_row  = cursor_row;
    undo_stack[undo_top].cursor_col  = cursor_col;
    undo_top++;
    modified = 1;
}

static void undo_pop(void) {
    if (undo_top <= 0) return;
    memcpy(redo_stack[redo_top].lines, lines, sizeof(lines));
    redo_stack[redo_top].line_count = line_count;
    redo_stack[redo_top].cursor_row = cursor_row;
    redo_stack[redo_top].cursor_col = cursor_col;
    redo_top++;
    undo_top--;
    memcpy(lines, undo_stack[undo_top].lines, sizeof(lines));
    line_count  = undo_stack[undo_top].line_count;
    cursor_row  = undo_stack[undo_top].cursor_row;
    cursor_col  = undo_stack[undo_top].cursor_col;
}

static void redo_pop(void) {
    if (redo_top <= 0) return;
    redo_top--;
    memcpy(lines, redo_stack[redo_top].lines, sizeof(lines));
    line_count  = redo_stack[redo_top].line_count;
    cursor_row  = redo_stack[redo_top].cursor_row;
    cursor_col  = redo_stack[redo_top].cursor_col;
}

/* ========== SECIM ========== */
static int sel_active = 0;
static int sel_start_row = 0, sel_start_col = 0;
static int sel_end_row   = 0, sel_end_col   = 0;

static char clipboard[CLIPBOARD_SZ] = "";

static int pos_cmp(int r1, int c1, int r2, int c2) {
    if (r1 != r2) return r1 < r2 ? -1 : 1;
    if (c1 != c2) return c1 < c2 ? -1 : 1;
    return 0;
}

static void sel_ordered(int *sr, int *sc, int *er, int *ec) {
    if (pos_cmp(sel_start_row, sel_start_col, sel_end_row, sel_end_col) <= 0) {
        *sr = sel_start_row; *sc = sel_start_col;
        *er = sel_end_row;   *ec = sel_end_col;
    } else {
        *sr = sel_end_row;   *sc = sel_end_col;
        *er = sel_start_row; *ec = sel_start_col;
    }
}

static int is_selected(int row, int col) {
    if (!sel_active) return 0;
    int sr, sc, er, ec;
    sel_ordered(&sr, &sc, &er, &ec);
    return pos_cmp(sr, sc, row, col) <= 0 && pos_cmp(row, col, er, ec) < 0;
}

static void copy_selection(void) {
    if (!sel_active) return;
    int sr, sc, er, ec;
    sel_ordered(&sr, &sc, &er, &ec);
    clipboard[0] = '\0';
    int i;
    for (i = sr; i <= er; i++) {
        int from = (i == sr) ? sc : 0;
        int to   = (i == er) ? ec : (int)strlen(lines[i]);
        strncat(clipboard, lines[i] + from, to - from);
        if (i < er) strcat(clipboard, "\n");
    }
}

static void delete_line_at(int row);

static void delete_selection(void) {
    if (!sel_active) return;
    undo_push();
    int sr, sc, er, ec;
    sel_ordered(&sr, &sc, &er, &ec);
    char tail[MAX_COLS];
    strncpy(tail, lines[er] + ec, MAX_COLS - 1); tail[MAX_COLS-1] = '\0';
    lines[sr][sc] = '\0';
    strncat(lines[sr], tail, MAX_COLS - 1 - sc);
    int del = er - sr, i;
    for (i = 0; i < del; i++) {
        int idx = sr + 1;
        if (idx < line_count) {
            int j;
            for (j = idx; j < line_count - 1; j++) strcpy(lines[j], lines[j+1]);
            memset(lines[line_count-1], 0, MAX_COLS);
            line_count--;
        }
    }
    cursor_row = sr; cursor_col = sc;
    sel_active = 0;
}

static void paste_clipboard(void) {
    if (!strlen(clipboard)) return;
    undo_push();
    char *p = clipboard;
    while (*p) {
        if (*p == '\n') {
            if (line_count < MAX_LINES - 1) {
                int i;
                for (i = line_count; i > cursor_row + 1; i--) strcpy(lines[i], lines[i-1]);
                strcpy(lines[cursor_row+1], lines[cursor_row] + cursor_col);
                lines[cursor_row][cursor_col] = '\0';
                line_count++; cursor_row++; cursor_col = 0;
            }
        } else {
            int clen = utf8_char_len((unsigned char)*p);
            int len  = (int)strlen(lines[cursor_row]);
            if (len + clen < MAX_COLS - 1) {
                memmove(&lines[cursor_row][cursor_col + clen],
                        &lines[cursor_row][cursor_col],
                        len - cursor_col + 1);
                int k;
                for (k = 0; k < clen && *(p+k); k++)
                    lines[cursor_row][cursor_col + k] = *(p + k);
                cursor_col += clen;
                p += (clen - 1);
            }
        }
        p++;
    }
    modified = 1;
}

/* ========== ARAMA ========== */
typedef struct { int row, col; } Match;
static Match matches[MAX_MATCHES];
static int   match_count = 0, current_match = -1;
static char  search_term[256] = "";

static void find_matches(const char *term) {
    match_count = 0; current_match = -1;
    if (!strlen(term)) return;
    int i;
    for (i = 0; i < line_count; i++) {
        char *ptr = lines[i];
        while ((ptr = strstr(ptr, term)) != NULL) {
            if (match_count < MAX_MATCHES) {
                matches[match_count].row = i;
                matches[match_count].col = (int)(ptr - lines[i]);
                match_count++;
            }
            ptr++;
        }
    }
    if (match_count > 0) current_match = 0;
}

static void replace_all(const char *old_t, const char *new_t) {
    int olen = (int)strlen(old_t), nlen = (int)strlen(new_t);
    int i;
    for (i = 0; i < line_count; i++) {
        char tmp[MAX_COLS]; char *src = lines[i], *dst = tmp;
        while (*src) {
            if (strncmp(src, old_t, olen) == 0) {
                strncpy(dst, new_t, nlen); dst += nlen; src += olen;
            } else { *dst++ = *src++; }
        }
        *dst = '\0';
        strcpy(lines[i], tmp);
    }
    modified = 1;
}

/* ========== KARAKTER ISLEMLERI ========== */
static void insert_char_at(int row, int col, char c) {
    int len = (int)strlen(lines[row]);
    if (len >= MAX_COLS - 1) return;
    memmove(&lines[row][col+1], &lines[row][col], len - col + 1);
    lines[row][col] = c;
}

static void delete_char_at(int row, int col) {
    int len = (int)strlen(lines[row]);
    if (col >= len) return;
    int clen = utf8_char_len((unsigned char)lines[row][col]);
    memmove(&lines[row][col], &lines[row][col + clen], len - col - clen + 1);
}

static void insert_newline(int row, int col) {
    if (line_count >= MAX_LINES - 1) return;
    int i;
    for (i = line_count; i > row + 1; i--) strcpy(lines[i], lines[i-1]);
    strcpy(lines[row+1], &lines[row][col]);
    lines[row][col] = '\0';
    line_count++;
}

static void delete_line_at(int row) {
    if (line_count <= 1) return;
    int i;
    for (i = row; i < line_count - 1; i++) strcpy(lines[i], lines[i+1]);
    memset(lines[line_count-1], 0, MAX_COLS);
    line_count--;
}

static void merge_with_above(int row) {
    if (row <= 0) return;
    int above_len = (int)strlen(lines[row-1]);
    strcat(lines[row-1], lines[row]);
    delete_line_at(row);
    cursor_row--; cursor_col = above_len;
}

static void delete_word_before(void) {
    if (cursor_col == 0) { merge_with_above(cursor_row); return; }
    int col = cursor_col;
    while (col > 0 && lines[cursor_row][utf8_prev(lines[cursor_row], col)] == ' ')
        col = utf8_prev(lines[cursor_row], col);
    while (col > 0 && lines[cursor_row][utf8_prev(lines[cursor_row], col)] != ' ')
        col = utf8_prev(lines[cursor_row], col);
    memmove(&lines[cursor_row][col],
            &lines[cursor_row][cursor_col],
            strlen(lines[cursor_row]) - cursor_col + 1);
    cursor_col = col;
}

/* ========== DOSYA ISLEMLERI ========== */
static void save_file(const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) return;
    int i;
    for (i = 0; i < line_count; i++) fprintf(f, "%s\n", lines[i]);
    fclose(f);
    strncpy(current_file, filename, MAX_PATH - 1);
    modified = 0;
}

static void load_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return;
    memset(lines, 0, sizeof(lines));
    line_count = 0;
    while (fgets(lines[line_count], MAX_COLS, f)) {
        int len = (int)strlen(lines[line_count]);
        if (len > 0 && lines[line_count][len-1] == '\n') lines[line_count][len-1] = '\0';
        line_count++;
    }
    fclose(f);
    if (line_count == 0) line_count = 1;
    strncpy(current_file, filename, MAX_PATH - 1);
    cursor_row = cursor_col = scroll_row = 0;
    modified = 0;
}

/* ========== EKRAN: LOW-LEVEL ========== */

/* Konsol boyutunu guncelle */
static void update_term_size(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    term_cols = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
    term_rows = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
}

/* Imleci belirtilen konuma tasimak icin VT sekans */
static void move_cursor(int row, int col) {
    /* ANSI/VT dizisi: ESC[row;colH (1 tabanli) */
    printf("\x1b[%d;%dH", row + 1, col + 1);
}

static void clear_screen(void) {
    printf("\x1b[2J");
    move_cursor(0, 0);
}

/* Renk/stil sabitleri (VT/ANSI) */
#define VT_RESET   "\x1b[0m"
#define VT_REVERSE "\x1b[7m"
#define VT_BG_BLUE "\x1b[44;97m"   /* Arama vurgusu: mavi zemin, beyaz yazi */
#define VT_BG_YELL "\x1b[43;30m"   /* Secim: sari zemin, siyah yazi */

/* Belirtilen konuma wchar_t karakteri UTF-8 olarak yaz */
static void print_wchar_at(int row, int vcol, wchar_t wc) {
    char buf[8] = {0};
    int  n = wchar_to_utf8(wc, buf);
    buf[n] = '\0';
    move_cursor(row, vcol);
    printf("%s", buf);
}

/* ========== EKRAN CIZIMI ========== */

static void draw_toolbox(void) {
    move_cursor(0, 0);
    printf("%s", VT_REVERSE);
    /* Tum sutunu doldur */
    int i;
    for (i = 0; i < term_cols; i++) printf(" ");
    move_cursor(0, 0);
    printf(" Ac(^O) Kaydet(^S) FarkliKaydet(^A) Ara(^F) Sonraki(^G) Onceki(^W) "
           "Degistir(^R) GeriAl(^Z) IleriAl(^Y) Kopyala(^C) Kes(^X) Yapistir(^V) "
           "Sec(^Yon) Cikis(ESC)");
    printf("%s", VT_RESET);
}

static void draw_editor(void) {
    int term_len = (int)strlen(search_term);
    int visible  = term_rows - 3;
    int i;

    for (i = 0; i < visible; i++) {
        int line_idx = i + scroll_row;
        move_cursor(i + 2, 0);

        /* Satir numarasi */
        printf("%s%3d | %s", VT_REVERSE, line_idx < line_count ? line_idx + 1 : 0, VT_RESET);

        if (line_idx >= line_count) continue;

        /* UTF-8 karakterleri tek tek ciz, renklendirme uygula */
        int j    = 0;
        int vcol = 6;  /* satir no + " | " = 6 karakter */
        int len  = (int)strlen(lines[line_idx]);

        while (j < len) {
            if (!utf8_is_start((unsigned char)lines[line_idx][j])) { j++; continue; }

            int clen = utf8_char_len((unsigned char)lines[line_idx][j]);
            int sel  = is_selected(line_idx, j);
            int fnd  = (!sel && term_len > 0 &&
                        strncmp(&lines[line_idx][j], search_term, term_len) == 0);

            if (fnd) {
                /* Arama eslesmesi: mavi */
                printf("%s", VT_BG_BLUE);
                int k = 0;
                while (k < term_len && j < len) {
                    if (!utf8_is_start((unsigned char)lines[line_idx][j])) { j++; k++; continue; }
                    int cl = utf8_char_len((unsigned char)lines[line_idx][j]);
                    wchar_t wc = utf8_to_wchar(lines[line_idx], j);
                    print_wchar_at(i + 2, vcol, wc); vcol++;
                    j += cl; k += cl;
                }
                printf("%s", VT_RESET);
            } else if (sel) {
                /* Secili metin: sari */
                printf("%s", VT_BG_YELL);
                wchar_t wc = utf8_to_wchar(lines[line_idx], j);
                print_wchar_at(i + 2, vcol, wc); vcol++;
                j += clen;
                printf("%s", VT_RESET);
            } else {
                /* Normal */
                wchar_t wc = utf8_to_wchar(lines[line_idx], j);
                print_wchar_at(i + 2, vcol, wc); vcol++;
                j += clen;
            }
        }
        /* Satirin geri kalanini temizle */
        printf("\x1b[K");
    }
}

static void draw_statusbar(void) {
    int vcol = utf8_byte_to_vcol(lines[cursor_row], cursor_col);
    move_cursor(term_rows - 1, 0);
    printf("%s", VT_REVERSE);
    printf(" Satir:%-4d Sutun:%-4d Dosya:%-30s%s",
           cursor_row + 1, vcol + 1,
           strlen(current_file) > 0 ? current_file : "[Yeni Dosya]",
           modified ? " [*]" : "    ");
    if (match_count > 0)
        printf("  Eslesen:%d/%d", current_match + 1, match_count);
    printf("\x1b[K%s", VT_RESET);
}

static void draw_all(void) {
    update_term_size();
    int visible = term_rows - 3;
    if (cursor_row < scroll_row)             scroll_row = cursor_row;
    if (cursor_row >= scroll_row + visible)  scroll_row = cursor_row - visible + 1;
    clear_screen();
    draw_toolbox();
    draw_editor();
    draw_statusbar();
    /* Imleci metin uzerinde dogru konuma getir */
    int vcol = utf8_byte_to_vcol(lines[cursor_row], cursor_col);
    move_cursor((cursor_row - scroll_row) + 2, vcol + 6);
    fflush(stdout);
}

/* ========== PROMPT: ALTTA GIRIS ALANi ========== */
/*
 * Windows Console API ile ReadConsoleInputW kullanarak UTF-16 karakterleri okur,
 * UTF-8'e donusturur. Bu sayede Turkce karakterler hic bozulmaz.
 */

typedef enum { PKind_None, PKind_Char, PKind_Backspace, PKind_Enter, PKind_Escape } PromptKey;

typedef struct { PromptKey kind; char utf8[8]; int len; } PromptKeyResult;

static PromptKeyResult read_prompt_key(void) {
    PromptKeyResult res = {PKind_None, {0}, 0};
    INPUT_RECORD ir;
    DWORD nr;
    while (1) {
        ReadConsoleInputW(hIn, &ir, 1, &nr);
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;
        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
        wchar_t wc = ir.Event.KeyEvent.uChar.UnicodeChar;
        if (vk == VK_RETURN)    { res.kind = PKind_Enter;     return res; }
        if (vk == VK_ESCAPE)    { res.kind = PKind_Escape;    return res; }
        if (vk == VK_BACK)      { res.kind = PKind_Backspace; return res; }
        if (wc >= 32) {
            /* UTF-16 -> UTF-8 donusumu */
            res.kind = PKind_Char;
            res.len  = wchar_to_utf8(wc, res.utf8);
            res.utf8[res.len] = '\0';
            return res;
        }
    }
}

/*
 * Altta prompt satiri gosterir, kullanicidan UTF-8 metin alir.
 * row: hangi terminal satirinda gosterilecek
 * Donus: 1 = Enter ile onaylandi, 0 = ESC ile iptal
 */
static int prompt_input(int row, const char *label, char *buf, int buf_size) {
    buf[0] = '\0';
    int byte_len = 0;
    move_cursor(row, 0);
    printf("%s%s%-50s%s", VT_REVERSE, label, "", VT_RESET);
    fflush(stdout);

    while (1) {
        PromptKeyResult k = read_prompt_key();
        if (k.kind == PKind_Enter)  return 1;
        if (k.kind == PKind_Escape) return 0;

        if (k.kind == PKind_Backspace && byte_len > 0) {
            /* UTF-8 farkinda backspace */
            int prev = byte_len - 1;
            while (prev > 0 && !utf8_is_start((unsigned char)buf[prev])) prev--;
            buf[prev] = '\0';
            byte_len = prev;
        } else if (k.kind == PKind_Char) {
            if (byte_len + k.len < buf_size - 1) {
                memcpy(buf + byte_len, k.utf8, k.len);
                byte_len += k.len;
                buf[byte_len] = '\0';
            }
        }

        move_cursor(row, 0);
        printf("%s%s%-50s%s", VT_REVERSE, label, buf, VT_RESET);
        fflush(stdout);
    }
}

/* ========== DOSYA YONETICISI ========== */
typedef struct { char name[MAX_PATH]; int is_dir; } FileEntry;
static FileEntry file_list[MAX_FILES];
static int       file_count = 0;

static void list_directory(const char *path) {
    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    file_count = 0;
    /* ".." her zaman ilk sirada */
    strcpy(file_list[file_count].name, "..");
    file_list[file_count].is_dir = 1;
    file_count++;
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (file_count >= MAX_FILES) break;
        strncpy(file_list[file_count].name, fd.cFileName, MAX_PATH - 1);
        file_list[file_count].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        file_count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void file_manager(char *result, int mod) {
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    int selected = 0, scroll = 0;

    while (1) {
        list_directory(cwd);
        update_term_size();
        clear_screen();

        move_cursor(0, 0);
        printf("%s %s - %s %s",
               VT_REVERSE,
               mod == 0 ? "DOSYA AC" : "DOSYA KAYDET",
               cwd, VT_RESET);

        int visible = term_rows - 4, i;
        for (i = 0; i < visible && (i + scroll) < file_count; i++) {
            int idx = i + scroll;
            move_cursor(i + 2, 2);
            if (idx == selected) printf("%s", VT_REVERSE);
            printf("%s %s",
                   file_list[idx].is_dir ? "[KLASOR]" : "        ",
                   file_list[idx].name);
            if (idx == selected) printf("%s", VT_RESET);
        }

        move_cursor(term_rows - 1, 0);
        printf("%s%s%s", VT_REVERSE,
               mod == 0
                   ? " Enter:Ac  Yukari/Asagi:Sec  Backspace:Ust Dizin  ESC:Iptal "
                   : " Enter:Sec/Gir  N:Yeni Dosya  Backspace:Ust Dizin  ESC:Iptal ",
               VT_RESET);
        fflush(stdout);

        /* Dosya yoneticisinde de ReadConsoleInputW kullan */
        INPUT_RECORD ir;
        DWORD nr;
        while (1) {
            ReadConsoleInputW(hIn, &ir, 1, &nr);
            if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) break;
        }
        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;

        if (vk == VK_ESCAPE)   { result[0] = '\0'; return; }
        if (vk == VK_UP)       { if (selected > 0) selected--; if (selected < scroll) scroll--; }
        if (vk == VK_DOWN)     { if (selected < file_count-1) selected++; if (selected >= scroll+visible) scroll++; }
        if (vk == VK_BACK)     { char *l = strrchr(cwd,'\\'); if (l && l!=cwd) *l='\0'; else { cwd[2]='\0'; } selected=0;scroll=0; }

        if (vk == VK_RETURN) {
            if (file_list[selected].is_dir) {
                if (!strcmp(file_list[selected].name, "..")) {
                    char *l = strrchr(cwd, '\\');
                    if (l && l != cwd) *l = '\0';
                    else { cwd[2] = '\0'; }  /* kok dizin */
                } else {
                    char np[MAX_PATH];
                    snprintf(np, MAX_PATH, "%s\\%s", cwd, file_list[selected].name);
                    strncpy(cwd, np, MAX_PATH - 1);
                }
                selected = 0; scroll = 0;
            } else {
                snprintf(result, MAX_PATH, "%s\\%s", cwd, file_list[selected].name);
                return;
            }
        }

        if (mod == 1 && (vk == 'N')) {
            char fname[MAX_PATH] = "";
            int ok = prompt_input(term_rows - 2, " Yeni dosya adi: ", fname, MAX_PATH);
            if (ok && strlen(fname) > 0) {
                snprintf(result, MAX_PATH, "%s\\%s", cwd, fname);
                return;
            }
        }
    }
}

/* ========== ARAMA / DEGISTIRME PROMPTS ========== */
static void search_prompt(void) {
    char term[256] = "";
    int ok = prompt_input(term_rows - 2, " Ara: ", term, sizeof(term));
    if (!ok || !strlen(term)) { search_term[0]='\0'; match_count=0; return; }
    strncpy(search_term, term, 255);
    find_matches(search_term);
    if (match_count > 0) { cursor_row = matches[0].row; cursor_col = matches[0].col; }
}

static void replace_prompt(void) {
    char old_t[256] = "", new_t[256] = "";
    int ok = prompt_input(term_rows - 3, " Degistirilecek: ", old_t, sizeof(old_t));
    if (!ok || !strlen(old_t)) return;
    ok = prompt_input(term_rows - 2, " Yeni kelime: ", new_t, sizeof(new_t));
    if (!ok) return;
    undo_push();
    replace_all(old_t, new_t);
    find_matches(old_t[0] ? new_t : "");
}

/* ========== ANA GIRIS OKUMA (ReadConsoleInputW) ========== */
typedef enum {
    EV_NONE,
    EV_CHAR,           /* normal karakter (Turkce dahil) */
    EV_ENTER,
    EV_BACKSPACE,
    EV_ESCAPE,
    EV_CTRL_BACKSPACE,
    EV_CTRL_F, EV_CTRL_G, EV_CTRL_W,
    EV_CTRL_H, EV_CTRL_R,
    EV_CTRL_S, EV_CTRL_A, EV_CTRL_O,
    EV_CTRL_C, EV_CTRL_X, EV_CTRL_V,
    EV_CTRL_Z, EV_CTRL_Y,
    EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT,
    EV_CTRL_UP, EV_CTRL_DOWN, EV_CTRL_LEFT, EV_CTRL_RIGHT,
    EV_SHIFT_UP, EV_SHIFT_DOWN, EV_SHIFT_LEFT, EV_SHIFT_RIGHT,
    EV_CTRL_SHIFT_LEFT, EV_CTRL_SHIFT_RIGHT,
    EV_CTRL_SHIFT_UP,   EV_CTRL_SHIFT_DOWN,
} EventKind;

typedef struct { EventKind kind; char utf8[8]; int len; } Event;

static Event read_event(void) {
    Event ev = {EV_NONE, {0}, 0};
    INPUT_RECORD ir;
    DWORD nr;
    while (1) {
        ReadConsoleInputW(hIn, &ir, 1, &nr);
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        DWORD  ctrl  = ir.Event.KeyEvent.dwControlKeyState;
        BOOL   shift = (ctrl & SHIFT_PRESSED)   != 0;
        BOOL   ckey  = (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        WORD   vk    = ir.Event.KeyEvent.wVirtualKeyCode;
        wchar_t wc   = ir.Event.KeyEvent.uChar.UnicodeChar;

        /* Yön tuslari */
        if (vk == VK_UP)    { ev.kind = ckey ? (shift ? EV_CTRL_SHIFT_UP    : EV_CTRL_UP)    : (shift ? EV_SHIFT_UP    : EV_UP);    return ev; }
        if (vk == VK_DOWN)  { ev.kind = ckey ? (shift ? EV_CTRL_SHIFT_DOWN  : EV_CTRL_DOWN)  : (shift ? EV_SHIFT_DOWN  : EV_DOWN);  return ev; }
        if (vk == VK_LEFT)  { ev.kind = ckey ? (shift ? EV_CTRL_SHIFT_LEFT  : EV_CTRL_LEFT)  : (shift ? EV_SHIFT_LEFT  : EV_LEFT);  return ev; }
        if (vk == VK_RIGHT) { ev.kind = ckey ? (shift ? EV_CTRL_SHIFT_RIGHT : EV_CTRL_RIGHT) : (shift ? EV_SHIFT_RIGHT : EV_RIGHT); return ev; }

        /* Ozel tuslar */
        if (vk == VK_RETURN)  { ev.kind = EV_ENTER;     return ev; }
        if (vk == VK_ESCAPE)  { ev.kind = EV_ESCAPE;    return ev; }
        if (vk == VK_BACK)    { ev.kind = ckey ? EV_CTRL_BACKSPACE : EV_BACKSPACE; return ev; }

        /* CTRL kombinasyonlari */
        if (ckey) {
            switch (vk) {
            case 'F': ev.kind = EV_CTRL_F; return ev;
            case 'G': ev.kind = EV_CTRL_G; return ev;
            case 'W': ev.kind = EV_CTRL_W; return ev;
            case 'H': ev.kind = EV_CTRL_H; return ev;
            case 'R': ev.kind = EV_CTRL_R; return ev;
            case 'S': ev.kind = EV_CTRL_S; return ev;
            case 'A': ev.kind = EV_CTRL_A; return ev;
            case 'O': ev.kind = EV_CTRL_O; return ev;
            case 'C': ev.kind = EV_CTRL_C; return ev;
            case 'X': ev.kind = EV_CTRL_X; return ev;
            case 'V': ev.kind = EV_CTRL_V; return ev;
            case 'Z': ev.kind = EV_CTRL_Z; return ev;
            case 'Y': ev.kind = EV_CTRL_Y; return ev;
            }
            continue; /* diger CTRL kombinasyonlarini yoksay */
        }

        /* Normal karakter (Turkce dahil) - wchar_t -> UTF-8 */
        if (wc >= 32) {
            ev.kind = EV_CHAR;
            ev.len  = wchar_to_utf8(wc, ev.utf8);
            ev.utf8[ev.len] = '\0';
            return ev;
        }
    }
}

/* ========== ANA FONKSIYON ========== */
int main(void) {
    /* Windows konsolunu UTF-8 + VT modu icin ayarla */
    SetConsoleCP(65001);
    SetConsoleOutputCP(65001);

    hIn  = GetStdHandle(STD_INPUT_HANDLE);
    hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    /* VT (ANSI kacis dizisi) destegini etkinlestir */
    DWORD out_mode;
    GetConsoleMode(hOut, &out_mode);
    SetConsoleMode(hOut, out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);

    /* Ham mod: karakterleri islenmeden al */
    DWORD in_mode;
    GetConsoleMode(hIn, &in_mode);
    SetConsoleMode(hIn, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);

    /* stdout'u tampon kullanmadan kullan */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Basit UTF-8 stdio ayari */
   // _setmode(_fileno(stdout), _O_U8TEXT);

    memset(lines, 0, sizeof(lines));

    int running = 1;
    while (running) {
        draw_all();

        Event ev = read_event();

        switch (ev.kind) {

        /* --- CIKIS --- */
        case EV_ESCAPE:
            search_term[0] = '\0'; match_count = 0; current_match = -1; sel_active = 0;
            if (modified) {
                draw_all();
                move_cursor(term_rows - 1, 0);
                printf("%s Kaydetmek istiyor musunuz? (y/n): %s", VT_REVERSE, VT_RESET);
                fflush(stdout);
                INPUT_RECORD ir2; DWORD nr2;
                ReadConsoleInputW(hIn, &ir2, 1, &nr2);
                while (!(ir2.EventType == KEY_EVENT && ir2.Event.KeyEvent.bKeyDown))
                    ReadConsoleInputW(hIn, &ir2, 1, &nr2);
                wchar_t ans = ir2.Event.KeyEvent.uChar.UnicodeChar;
                if (ans == L'y' || ans == L'Y') {
                    if (strlen(current_file) > 0) save_file(current_file);
                    else { char fn[MAX_PATH]=""; file_manager(fn,1); if(strlen(fn)>0) save_file(fn); }
                    running = 0;
                } else if (ans == L'n' || ans == L'N') { running = 0; }
            } else { running = 0; }
            break;

        /* --- UNDO / REDO --- */
        case EV_CTRL_Z: undo_pop(); sel_active = 0; break;
        case EV_CTRL_Y: redo_pop(); sel_active = 0; break;

        /* --- ARAMA --- */
        case EV_CTRL_F: search_prompt(); break;
        case EV_CTRL_G:
            if (match_count > 0) {
                current_match = (current_match + 1) % match_count;
                cursor_row = matches[current_match].row;
                cursor_col = matches[current_match].col;
            }
            break;
        case EV_CTRL_W:
            if (match_count > 0) {
                current_match = (current_match - 1 + match_count) % match_count;
                cursor_row = matches[current_match].row;
                cursor_col = matches[current_match].col;
            }
            break;
        case EV_CTRL_H:
        case EV_CTRL_R: replace_prompt(); break;

        /* --- DOSYA ISLEMLERI --- */
        case EV_CTRL_S:
            if (strlen(current_file) > 0) save_file(current_file);
            else { char fn[MAX_PATH]=""; file_manager(fn,1); if(strlen(fn)>0) save_file(fn); }
            break;
        case EV_CTRL_A: { char fn[MAX_PATH]=""; file_manager(fn,1); if(strlen(fn)>0) save_file(fn); } break;
        case EV_CTRL_O: { char fn[MAX_PATH]=""; file_manager(fn,0); if(strlen(fn)>0) load_file(fn); } break;

        /* --- KOPYALA / KES / YAPISTIR --- */
        case EV_CTRL_C: copy_selection(); sel_active = 0; break;
        case EV_CTRL_X: copy_selection(); delete_selection(); break;
        case EV_CTRL_V: paste_clipboard(); break;

        /* --- YON TUSLARI --- */
        case EV_LEFT:
            sel_active = 0;
            if (cursor_col > 0) cursor_col = utf8_prev(lines[cursor_row], cursor_col);
            else if (cursor_row > 0) { cursor_row--; cursor_col = (int)strlen(lines[cursor_row]); }
            break;
        case EV_RIGHT:
            sel_active = 0;
            { int len=(int)strlen(lines[cursor_row]);
              if (cursor_col < len) cursor_col = utf8_next(lines[cursor_row], cursor_col);
              else if (cursor_row < line_count-1) { cursor_row++; cursor_col=0; } }
            break;
        case EV_UP:
            sel_active = 0;
            if (cursor_row > 0) {
                int vc = utf8_byte_to_vcol(lines[cursor_row], cursor_col);
                cursor_row--;
                cursor_col = utf8_vcol_to_byte(lines[cursor_row], vc);
                int nl = (int)strlen(lines[cursor_row]);
                if (cursor_col > nl) cursor_col = nl;
            }
            break;
        case EV_DOWN:
            sel_active = 0;
            if (cursor_row < line_count - 1) {
                int vc = utf8_byte_to_vcol(lines[cursor_row], cursor_col);
                cursor_row++;
                cursor_col = utf8_vcol_to_byte(lines[cursor_row], vc);
                int nl = (int)strlen(lines[cursor_row]);
                if (cursor_col > nl) cursor_col = nl;
            }
            break;

        /* CTRL + YON: karakter bazli secim */
        case EV_CTRL_LEFT:
            if (!sel_active) { sel_active=1; sel_start_row=cursor_row; sel_start_col=cursor_col; }
            if (cursor_col > 0) cursor_col = utf8_prev(lines[cursor_row], cursor_col);
            else if (cursor_row > 0) { cursor_row--; cursor_col=(int)strlen(lines[cursor_row]); }
            sel_end_row=cursor_row; sel_end_col=cursor_col;
            break;
        case EV_CTRL_RIGHT:
            if (!sel_active) { sel_active=1; sel_start_row=cursor_row; sel_start_col=cursor_col; }
            { int len=(int)strlen(lines[cursor_row]);
              if (cursor_col < len) cursor_col = utf8_next(lines[cursor_row], cursor_col);
              else if (cursor_row < line_count-1) { cursor_row++; cursor_col=0; } }
            sel_end_row=cursor_row; sel_end_col=cursor_col;
            break;
        case EV_CTRL_UP:
            if (!sel_active) { sel_active=1; sel_start_row=cursor_row; sel_start_col=cursor_col; }
            if (cursor_row > 0) {
                int vc=utf8_byte_to_vcol(lines[cursor_row],cursor_col); cursor_row--;
                cursor_col=utf8_vcol_to_byte(lines[cursor_row],vc);
            }
            sel_end_row=cursor_row; sel_end_col=cursor_col;
            break;
        case EV_CTRL_DOWN:
            if (!sel_active) { sel_active=1; sel_start_row=cursor_row; sel_start_col=cursor_col; }
            if (cursor_row < line_count-1) {
                int vc=utf8_byte_to_vcol(lines[cursor_row],cursor_col); cursor_row++;
                cursor_col=utf8_vcol_to_byte(lines[cursor_row],vc);
            }
            sel_end_row=cursor_row; sel_end_col=cursor_col;
            break;

        /* CTRL+SHIFT+YON: kelime bazli secim */
        case EV_CTRL_SHIFT_RIGHT:
            if (!sel_active) { sel_active=1; sel_start_row=cursor_row; sel_start_col=cursor_col; }
            { int len=(int)strlen(lines[cursor_row]);
              while(cursor_col<len && lines[cursor_row][cursor_col]==' ')
                  cursor_col=utf8_next(lines[cursor_row],cursor_col);
              while(cursor_col<len && lines[cursor_row][cursor_col]!=' ')
                  cursor_col=utf8_next(lines[cursor_row],cursor_col); }
            sel_end_row=cursor_row; sel_end_col=cursor_col;
            break;
        case EV_CTRL_SHIFT_LEFT:
            if (!sel_active) { sel_active=1; sel_start_row=cursor_row; sel_start_col=cursor_col; }
            while(cursor_col>0 && lines[cursor_row][utf8_prev(lines[cursor_row],cursor_col)]==' ')
                cursor_col=utf8_prev(lines[cursor_row],cursor_col);
            while(cursor_col>0 && lines[cursor_row][utf8_prev(lines[cursor_row],cursor_col)]!=' ')
                cursor_col=utf8_prev(lines[cursor_row],cursor_col);
            sel_end_row=cursor_row; sel_end_col=cursor_col;
            break;
        case EV_CTRL_SHIFT_UP:
            if (!sel_active) { sel_active=1; sel_start_row=cursor_row; sel_start_col=cursor_col; }
            if (cursor_row>0) { int vc=utf8_byte_to_vcol(lines[cursor_row],cursor_col); cursor_row--;
                cursor_col=utf8_vcol_to_byte(lines[cursor_row],vc); }
            sel_end_row=cursor_row; sel_end_col=cursor_col;
            break;
        case EV_CTRL_SHIFT_DOWN:
            if (!sel_active) { sel_active=1; sel_start_row=cursor_row; sel_start_col=cursor_col; }
            if (cursor_row<line_count-1) { int vc=utf8_byte_to_vcol(lines[cursor_row],cursor_col); cursor_row++;
                cursor_col=utf8_vcol_to_byte(lines[cursor_row],vc); }
            sel_end_row=cursor_row; sel_end_col=cursor_col;
            break;

        /* SHIFT+YON: tek karakter secim */
        case EV_SHIFT_LEFT:
            if (!sel_active) { sel_active=1; sel_start_row=cursor_row; sel_start_col=cursor_col; }
            if (cursor_col>0) cursor_col=utf8_prev(lines[cursor_row],cursor_col);
            else if (cursor_row>0) { cursor_row--; cursor_col=(int)strlen(lines[cursor_row]); }
            sel_end_row=cursor_row; sel_end_col=cursor_col;
            break;
        case EV_SHIFT_RIGHT:
            if (!sel_active) { sel_active=1; sel_start_row=cursor_row; sel_start_col=cursor_col; }
            { int len=(int)strlen(lines[cursor_row]);
              if (cursor_col<len) cursor_col=utf8_next(lines[cursor_row],cursor_col);
              else if (cursor_row<line_count-1) { cursor_row++; cursor_col=0; } }
            sel_end_row=cursor_row; sel_end_col=cursor_col;
            break;
        case EV_SHIFT_UP:
            if (!sel_active) { sel_active=1; sel_start_row=cursor_row; sel_start_col=cursor_col; }
            if (cursor_row>0) { int vc=utf8_byte_to_vcol(lines[cursor_row],cursor_col); cursor_row--;
                cursor_col=utf8_vcol_to_byte(lines[cursor_row],vc); }
            sel_end_row=cursor_row; sel_end_col=cursor_col;
            break;
        case EV_SHIFT_DOWN:
            if (!sel_active) { sel_active=1; sel_start_row=cursor_row; sel_start_col=cursor_col; }
            if (cursor_row<line_count-1) { int vc=utf8_byte_to_vcol(lines[cursor_row],cursor_col); cursor_row++;
                cursor_col=utf8_vcol_to_byte(lines[cursor_row],vc); }
            sel_end_row=cursor_row; sel_end_col=cursor_col;
            break;

        /* --- ENTER --- */
        case EV_ENTER:
            sel_active = 0;
            undo_push();
            insert_newline(cursor_row, cursor_col);
            cursor_row++; cursor_col = 0;
            modified = 1;
            break;

        /* --- SILME --- */
        case EV_CTRL_BACKSPACE:
            undo_push(); delete_word_before(); modified = 1;
            break;
        case EV_BACKSPACE:
            if (sel_active) { delete_selection(); }
            else {
                undo_push();
                if (cursor_col > 0) {
                    cursor_col = utf8_prev(lines[cursor_row], cursor_col);
                    delete_char_at(cursor_row, cursor_col);
                } else { merge_with_above(cursor_row); }
                modified = 1;
            }
            break;

        /* --- KARAKTER YAZMA ---
           ReadConsoleInputW sayesinde wc zaten UTF-16'da,
           wchar_to_utf8 ile UTF-8'e donusturulmus durumda. */
        case EV_CHAR:
            if (sel_active) delete_selection();
            undo_push();
            {
                int k;
                for (k = 0; k < ev.len; k++) {
                    insert_char_at(cursor_row, cursor_col, ev.utf8[k]);
                    cursor_col++;
                }
            }
            modified = 1;
            break;

        default: break;
        }
    }

    /* Temizlik */
    clear_screen();
    /* Konsolu eski moduna dondur */
    SetConsoleMode(hIn,  in_mode);
    SetConsoleMode(hOut, out_mode);
    return 0;
}
