#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app/view.h"

bool capture_copy_screen(view_t *view);
bool capture_save_screenshot(view_t *view, char *path, size_t size);
