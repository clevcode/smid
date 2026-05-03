#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum {
    LOG_QUEUE_CAP = 1024,
    LOG_MSG_CAP = 4096,
};

struct log_msg {
    size_t len;
    char data[LOG_MSG_CAP];
};

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t log_cond = PTHREAD_COND_INITIALIZER;
static pthread_t log_thread;
static struct log_msg log_queue[LOG_QUEUE_CAP];
static unsigned log_head;
static unsigned log_tail;
static unsigned log_count;
static unsigned log_dropped;
static bool log_running;
static bool log_stop_requested;

static void write_all_stderr(const char *data, size_t len) {
    while (len) {
        ssize_t n = write(STDERR_FILENO, data, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        data += (size_t)n;
        len -= (size_t)n;
    }
}

static void *log_thread_main(void *arg) {
    (void)arg;
    for (;;) {
        struct log_msg msg;
        unsigned dropped = 0;

        pthread_mutex_lock(&log_lock);
        while (!log_stop_requested && log_count == 0 && log_dropped == 0) {
            pthread_cond_wait(&log_cond, &log_lock);
        }
        if (log_stop_requested && log_count == 0 && log_dropped == 0) {
            pthread_mutex_unlock(&log_lock);
            break;
        }
        if (log_dropped) {
            dropped = log_dropped;
            log_dropped = 0;
            pthread_mutex_unlock(&log_lock);
            char buf[96];
            int n = snprintf(buf, sizeof(buf), "log: dropped %u messages\n", dropped);
            if (n > 0) {
                write_all_stderr(buf, (size_t)n);
            }
            continue;
        }
        msg = log_queue[log_tail];
        log_tail = (log_tail + 1u) % LOG_QUEUE_CAP;
        log_count--;
        pthread_mutex_unlock(&log_lock);

        write_all_stderr(msg.data, msg.len);
    }
    return NULL;
}

int smid_log_start(void) {
    pthread_mutex_lock(&log_lock);
    if (log_running) {
        pthread_mutex_unlock(&log_lock);
        return 0;
    }
    log_stop_requested = false;
    log_head = 0;
    log_tail = 0;
    log_count = 0;
    log_dropped = 0;
    pthread_mutex_unlock(&log_lock);

    if (pthread_create(&log_thread, NULL, log_thread_main, NULL)) {
        return -1;
    }

    pthread_mutex_lock(&log_lock);
    log_running = true;
    pthread_mutex_unlock(&log_lock);
    return 0;
}

void smid_log_stop(void) {
    pthread_mutex_lock(&log_lock);
    if (!log_running) {
        pthread_mutex_unlock(&log_lock);
        return;
    }
    log_stop_requested = true;
    pthread_cond_signal(&log_cond);
    pthread_mutex_unlock(&log_lock);
    pthread_join(log_thread, NULL);

    pthread_mutex_lock(&log_lock);
    log_running = false;
    pthread_mutex_unlock(&log_lock);
}

void smid_vlogf(const char *fmt, va_list ap) {
    char buf[LOG_MSG_CAP];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) {
        return;
    }
    size_t len = (size_t)n;
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1u;
        size_t fmt_len = strlen(fmt);
        if (len && fmt_len && fmt[fmt_len - 1u] == '\n') {
            buf[len - 1u] = '\n';
        }
    }

    pthread_mutex_lock(&log_lock);
    if (!log_running) {
        pthread_mutex_unlock(&log_lock);
        write_all_stderr(buf, len);
        return;
    }
    if (log_count == LOG_QUEUE_CAP) {
        log_tail = (log_tail + 1u) % LOG_QUEUE_CAP;
        log_count--;
        log_dropped++;
    }
    log_queue[log_head].len = len;
    memcpy(log_queue[log_head].data, buf, len);
    log_head = (log_head + 1u) % LOG_QUEUE_CAP;
    log_count++;
    pthread_cond_signal(&log_cond);
    pthread_mutex_unlock(&log_lock);
}

void smid_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    smid_vlogf(fmt, ap);
    va_end(ap);
}
