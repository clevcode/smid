#ifndef SMID_LOG_H
#define SMID_LOG_H

#include <stdarg.h>

int smid_log_start(void);
void smid_log_stop(void);
void smid_logf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void smid_vlogf(const char *fmt, va_list ap);

#endif

