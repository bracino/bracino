#pragma once

#include <stdint.h>

/* v0.08 netlist: ST7735S on SPI2. CS tied to GND. LED always on via R3. */
#define TFT_PIN_SCK   9
#define TFT_PIN_SDA   4
#define TFT_PIN_DC    3
#define TFT_PIN_RST   2

/* Landscape 160x128. 8x16 cells → 20 columns, 8 rows.
 * Col 0 is left blank (clip); text starts at column 1. */
#define TFT_W         160
#define TFT_H         128
#define TFT_COLS      20
#define TFT_ROWS      8
#define TFT_CHAR_W    8
#define TFT_CHAR_H    16
#define TFT_TEXT_X    0

/* RGB565 constants. MADCTL is BGR on this panel, so R and B nibbles are
 * swapped versus the usual Adafruit RGB wire order — otherwise amber/red
 * read as deep blue and cyan headers go yellowish. */
#define COL_RGB565(r, g, b) \
    ((uint16_t)((((b) & 0x1F) << 11) | (((g) & 0x3F) << 5) | ((r) & 0x1F)))

#define COL_BG        0x0000
#define COL_FG        0xFFFF
#define COL_HEADER    COL_RGB565(0, 63, 31)   /* cyan */
#define COL_FOOTER    COL_RGB565(0, 63, 31)   /* cyan */
#define COL_FOCUS     COL_RGB565(31, 63, 0)   /* yellow */
#define COL_EDIT      COL_RGB565(31, 40, 0)   /* amber */
#define COL_DIM       COL_RGB565(16, 32, 16)  /* grey */
#define COL_OK        COL_RGB565(0, 63, 0)    /* green */
#define COL_WARN      COL_RGB565(31, 40, 0)   /* amber */
#define COL_BAD       COL_RGB565(31, 0, 0)    /* red */

void tft_init(void);
void tft_fill(uint16_t color);
void tft_fill_rect(int x, int y, int w, int h, uint16_t color);
void tft_char(int x, int y, char c, uint16_t fg, uint16_t bg);
void tft_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);
void tft_text_row(int row, const char *s, uint16_t fg, uint16_t bg);
