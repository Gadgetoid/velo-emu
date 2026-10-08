#pragma once

#include <stdbool.h>

bool        menu_state_enabled(int item);
bool        menu_state_checked(int item);
bool        menu_state_hidden(int item);
const char *menu_state_title(int item, const char *fallback);
