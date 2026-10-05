#include <SDL3/SDL.h>

#include "app/android.h"

#define COMMAND_BRIGHTNESS     0x8000
#define COMMAND_KEEP_SCREEN_ON 0x8001
#define FULL_BRIGHTNESS        1000
#define SYSTEM_BRIGHTNESS      (-1)

void android_update(bool backlit, bool awake) {
    static int brightness = 0, keep_screen_on = -1;
    int wanted_brightness = backlit ? FULL_BRIGHTNESS : SYSTEM_BRIGHTNESS;
    if (wanted_brightness != brightness && SDL_SendAndroidMessage(COMMAND_BRIGHTNESS, wanted_brightness)) brightness = wanted_brightness;
    if ((int)awake != keep_screen_on && SDL_SendAndroidMessage(COMMAND_KEEP_SCREEN_ON, awake)) keep_screen_on = awake;
}
