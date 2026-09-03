#define _POSIX_C_SOURCE 200809L

#include "logger.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

bool gwd_logs_prepare(char *error, size_t error_size)
{
    if (mkdir("logs", 0777) == 0 || errno == EEXIST)
        return true;
    if (error != NULL && error_size != 0)
        snprintf(error, error_size, "cannot create logs directory: %s",
                 strerror(errno));
    return false;
}

bool gwd_logger_open(GwdLogger *logger, unsigned thread_index,
                     char *error, size_t error_size)
{
    char path[64];

    if (logger == NULL)
        return false;
    logger->file = NULL;
    logger->thread_index = thread_index;
    snprintf(path, sizeof(path), "logs/log%u.txt", thread_index);
    logger->file = fopen(path, "w");
    if (logger->file == NULL) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "cannot open %s: %s", path,
                     strerror(errno));
        return false;
    }
    return true;
}

void gwd_logger_log(GwdLogger *logger, const char *format, ...)
{
    struct timespec now;
    struct tm local;
    char timestamp[40];
    va_list arguments;

    if (logger == NULL || logger->file == NULL)
        return;
    clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &local);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    fprintf(logger->file, "[%s.%03ld] [thread %u] ", timestamp,
            now.tv_nsec / 1000000L, logger->thread_index);
    va_start(arguments, format);
    vfprintf(logger->file, format, arguments);
    va_end(arguments);
    fputc('\n', logger->file);
    fflush(logger->file);
}

void gwd_logger_close(GwdLogger *logger)
{
    if (logger == NULL || logger->file == NULL)
        return;
    fclose(logger->file);
    logger->file = NULL;
}
