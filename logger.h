#ifndef GWD9_LOGGER_H
#define GWD9_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    FILE *file;
    unsigned thread_index;
} GwdLogger;

bool gwd_logs_prepare(char *error, size_t error_size);
bool gwd_logger_open(GwdLogger *logger, unsigned thread_index,
                     char *error, size_t error_size);
void gwd_logger_log(GwdLogger *logger, const char *format, ...);
void gwd_logger_close(GwdLogger *logger);

#endif
