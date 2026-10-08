#include "app/host.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

bool host_confirm(SDL_Window *window, const char *title, const char *message, const char *action) {
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, action },
    };
    const SDL_MessageBoxData dialog = { SDL_MESSAGEBOX_WARNING, window, title, message, (int)(sizeof buttons / sizeof buttons[0]), buttons, NULL };
    int chosen = 0;
    return SDL_ShowMessageBox(&dialog, &chosen) && chosen == 1;
}

void host_open_path(const char *path) {
    char url[4096] = "file://";
    size_t length = strlen(url);
    for (const unsigned char *at = (const unsigned char *)path; *at && length + 4 < sizeof url; at++) {
        if (isalnum(*at) || strchr("/-_.~", *at)) url[length++] = (char)*at;
        else length += (size_t)snprintf(url + length, sizeof url - length, "%%%02X", *at);
    }
    url[length] = 0;
    SDL_OpenURL(url);
}

#define REVEAL_CHILDREN_MAX 16

static pid_t reveal_children[REVEAL_CHILDREN_MAX];
static int reveal_child_count = 0;

void host_reap_children(void) {
    int kept = 0;
    for (int i = 0; i < reveal_child_count; i++) {
        if (waitpid(reveal_children[i], NULL, WNOHANG) == 0) reveal_children[kept++] = reveal_children[i];
    }
    reveal_child_count = kept;
}

void host_reveal_file(const char *path) {
#ifdef __APPLE__
    extern char **environ;
    char *arguments[] = { "open", "-R", (char *)path, NULL };
    pid_t pid;
    if (posix_spawnp(&pid, "open", NULL, NULL, arguments, environ) != 0) return;
    if (reveal_child_count < REVEAL_CHILDREN_MAX) reveal_children[reveal_child_count++] = pid;
    else waitpid(pid, NULL, 0);
#else
    char folder[1100];
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    if (slash && slash != folder) *slash = 0;
    host_open_path(folder);
#endif
}

void host_local_address(char *address, size_t size) {
    snprintf(address, size, "this computer");
    struct ifaddrs *interfaces;
    if (getifaddrs(&interfaces) != 0) return;
    int best = 0;
    for (struct ifaddrs *at = interfaces; at; at = at->ifa_next) {
        if (!at->ifa_addr || at->ifa_addr->sa_family != AF_INET || (at->ifa_flags & IFF_LOOPBACK) || !(at->ifa_flags & IFF_UP)) continue;
        int score = !strncmp(at->ifa_name, "wlan", 4) || !strcmp(at->ifa_name, "en0") ? 2 : 1;
        if (score <= best) continue;
        best = score;
        inet_ntop(AF_INET, &((struct sockaddr_in *)at->ifa_addr)->sin_addr, address, (socklen_t)size);
    }
    freeifaddrs(interfaces);
}
