#ifndef RAMDISK_LOG_H
#define RAMDISK_LOG_H

#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

static inline const char *ramdisk_log_level_name(int level)
{
    switch (level) {
    case 0:
        return "ERR";
    case 1:
        return "WARN";
    case 2:
        return "INFO";
    default:
        return "DBG";
    }
}

static inline void ramdisk_log_emit(int level, const char *file, int line,
                                    const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(stderr, "[%ld.%03ld] %s %s:%d ",
            (long)ts.tv_sec, ts.tv_nsec / 1000000,
            ramdisk_log_level_name(level), file, line);

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define RD_LOG_ERR(...) ramdisk_log_emit(0, __FILE__, __LINE__, __VA_ARGS__)
#define RD_LOG_WARN(...) ramdisk_log_emit(1, __FILE__, __LINE__, __VA_ARGS__)
#define RD_LOG_INFO(...) ramdisk_log_emit(2, __FILE__, __LINE__, __VA_ARGS__)
#define RD_LOG_DBG(...) ramdisk_log_emit(3, __FILE__, __LINE__, __VA_ARGS__)

#define RD_LOG_ERRNO(msg) \
    RD_LOG_ERR("%s: errno=%d (%s)", (msg), errno, strerror(errno))

#endif
