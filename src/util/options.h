#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *name;
    const char *value;
    const char *help;
    int repeat;
} option_t;

typedef bool (*option_fn)(void *context, int option, const char *value, char *error, size_t error_size);

typedef struct {
    const char     *program;
    const char     *synopsis;
    const char     *summary;
    const option_t *options;
    int count;
    const char     *footer;
} option_spec_t;

typedef enum { OPTIONS_OK, OPTIONS_EXIT, OPTIONS_ERROR } options_result_t;

options_result_t options_parse(const option_spec_t *spec, int argc, char **argv, option_fn handle, void *context,
                               const char **positional, int max_positional, int *positional_count);
void             options_usage(const option_spec_t *spec);
const char      *options_version(void);

bool option_number(const char *text, double *value);
bool option_integer(const char *text, int base, long *value);
bool option_timed(const char *text, double *seconds, const char **rest);
