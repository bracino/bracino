#pragma once

#include <stdint.h>

/* v0.08 netlist: ST7735S on SPI2. CS tied to GND. LED always on via R3. */
#define TFT_PIN_SCK   9
#define TFT_PIN_SDA   4
#define TFT_PIN_DC    3
#define TFT_PIN_RST   2

/* Landscape 160x128. 8x16 cells → 20 columns, 8 rows.
 * TEXT_X 8: MY|MV + software X flip draws logical col 0 one cell past the
 * left edge; inset so the first glyph is on glass. Trailing pad col unused.
 */
#define TFT_W         160
#define TFT_H         128
#define TFT_COLS      20
#define TFT_ROWS      8
#define TFT_CHAR_W    8
#define TFT_CHAR_H    16
#define TFT_TEXT_X    8

#define COL_BG        0x0000
#define COL_FG        0xFFFF
#define COL_HEADER    0x07FF
#define COL_FOOTER    0x07FF
#define COL_FOCUS     0xFFE0
#define COL_EDIT      0xFD20
#define COL_DIM       0x8410
#define COL_OK        0x07E0
#define COL_WARN      0xFD20
#define COL_BAD       0xF800

void tft_init(void);
void tft_fill(uint16_t color);
void tft_fill_rect(int x, int y, int w, int h, uint16_t color);
void tft_char(int x, int y, char c, uint16_t fg, uint16_t bg);
void tft_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);
void tft_text_row(int row, const char *s, uint16_t fg, uint16_t bg);
