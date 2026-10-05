#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "core/screen.h"

#define LCD_MARGIN_X 4
#define LCD_MARGIN_Y 4

extern uint8_t lcd_framebuffer[SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT];

void      lcd_set_size(int width, int height);
int       lcd_width(void);
int       lcd_height(void);

void      lcd_compose_setup(int cell);
bool      lcd_compose(float seconds);
void      lcd_compose_dirty(int *x, int *y, int *width, int *height);
void      lcd_invalidate(void);
bool      lcd_needs_compose(void);
uint32_t *lcd_compose_pixels(void);
int       lcd_compose_width(void);
int       lcd_compose_height(void);
void      lcd_set_backlight(bool on);
void      lcd_set_unlit_level(float level);
bool      lcd_get_backlight(void);
void      lcd_set_power(bool on);
void      lcd_set_response(float scale);
void      lcd_set_contrast(int level);
int       lcd_get_contrast(void);
