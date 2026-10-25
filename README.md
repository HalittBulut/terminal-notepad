# Terminal Notepad

Windows konsolunda çalışan, C ile sıfırdan yazılmış bir metin editörü. Programlama Laboratuvarı kapsamında iki kişilik takımla geliştirildi.

![C](https://img.shields.io/badge/C-A8B9CC?logo=c&logoColor=black)
![Windows](https://img.shields.io/badge/Windows%20Console-0078D6?logo=windows&logoColor=white)

## Özellikler

- Konsol içinde dosya yöneticisiyle açma, kaydetme ve farklı kaydetme
- Ok tuşlarıyla gezinme, `Ctrl` ile kelime kelime atlama
- `Shift` ile metin seçme
- Kes, kopyala, yapıştır
- Geri al / yinele (100 adıma kadar)
- Arama, eşleşmeler arasında ileri-geri gezinme
- Bul ve değiştir

## Kısayollar

| Tuş | İşlev |
|---|---|
| Ctrl+O | Aç |
| Ctrl+S / Ctrl+A | Kaydet / farklı kaydet |
| Ctrl+C / Ctrl+X / Ctrl+V | Kopyala / kes / yapıştır |
| Ctrl+Z / Ctrl+Y | Geri al / yinele |
| Ctrl+F | Ara |
| Ctrl+G / Ctrl+W | Sonraki / önceki eşleşme |
| Ctrl+R veya Ctrl+H | Değiştir |

## Teknik

- Klavye girdisi Windows Console API ile doğrudan okunur (`ReadConsoleInput`), `Ctrl` ve `Shift` kombinasyonları olay türlerine çevrilir
- Satır tabanlı metin tamponu, sabit boyutlu geri alma yığını

## Derleme

```bash
gcc main.c -o notepad.exe
```

## Ekip

- [Halit Bulut](https://github.com/HalittBulut)
- Enes Canlı
