#ifndef BRAMBLE_DISPLAY_H
#define BRAMBLE_DISPLAY_H
#include <stdint.h>
// SSD1306 128x64 OLED driver — ESP32 I2C implementation
// Stub: implementation requires ESP-IDF I2C driver
int display_init(void);
void display_clear(void);
void display_draw_text(int x, int y, const char *text);
void display_flush(void);
#endif
