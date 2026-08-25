#pragma once

#include <stdint.h>

/* v0.08 netlist: bit-bang ST7735S, CS tied to GND. LED always on via R3. */
#define TFT_PIN_SCK   9
#define TFT_PIN_SDA   4
#define TFT_PIN_DC    3
#define TFT_PIN_RST   2

#define TFT_W         128
#define TFT_H         160

#define COL_BG        0x0000
#define COL_FG        0xFFFF
#define COL_DIM       0x8410
#define COL_ACCENT    0xFFE0
#define COL_OK        0x07E0
#define COL_WARN      0xFD20
#define COL_BAD       0xF800

void tft_init(void);
void tft_fill(uint16_t color);
void tft_fill_rect(int x, int y, int w, int h, uint16_t color);
void tft_char(int x, int y, char c, uint16_t fg, uint16_t bg);
void tft_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);
void tft_text_row(int row, const char *s, uint16_t fg, uint16_t bg);
