#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/screen.h"

#define PROFILES_MAX 16

typedef struct {
    char id[64];
    char name[96];
    char rom[1024];
    char state[1024];
    screen_size_t screen;
    uint32_t memory;
    bool host_time;
} profile_t;

typedef struct {
    profile_t entries[PROFILES_MAX];
    int count;
} profiles_t;

void profiles_load(profiles_t *profiles, const char *folder);
bool profile_save(const profile_t *profile, const char *folder);
bool profile_delete(const profile_t *profile, const char *folder);
int  profile_find(const profiles_t *profiles, const char *id_or_name);
void profile_default_name(const profile_t *profile, int system, char *name, size_t size);
void profile_make_unique(const profiles_t *profiles, profile_t *profile, const char *folder);
