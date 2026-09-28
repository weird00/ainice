#include "eventlog.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ha_log_entry_t g_ring[HA_LOG_CAP];
static int g_next;  /* 下一个写入位置 */
static int g_count; /* 有效条数 */
static char g_path[512];

static void log_path(char *buf, size_t size)
{
    const char *dir = getenv("AINICE_PLUGIN_DATA");

    if (dir == NULL || *dir == '\0') {
        dir = ".";
    }
    snprintf(buf, size, "%s/events.log", dir);
}

static void now_text(char *buf, size_t size)
{
    time_t t = time(NULL);
    struct tm tmv;

    if (localtime_r(&t, &tmv) == NULL) {
        snprintf(buf, size, "-");
        return;
    }
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tmv);
}

/* 只保留可打印字符，保证一行一条、JSON 转义简单。 */
static void flat_copy(char *dst, size_t size, const char *src)
{
    size_t di = 0;

    while (*src != '\0' && di + 1 < size) {
        unsigned char c = (unsigned char)*src++;

        dst[di++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    dst[di] = '\0';
}

static void json_escape(const char *src, char *dst, size_t size)
{
    size_t di = 0;

    while (*src != '\0' && di + 7 < size) {
        unsigned char c = (unsigned char)*src++;

        if (c == '"' || c == '\\') {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c < 0x20) {
            di += (size_t)snprintf(dst + di, size - di, "\\u%04x", c);
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

/* 调用方必须持锁。ts 为 NULL 时使用当前时间。 */
static void ring_push(const char *kind, const char *text, const char *ts)
{
    ha_log_entry_t *slot = &g_ring[g_next];

    flat_copy(slot->kind, sizeof slot->kind, kind);
    flat_copy(slot->text, sizeof slot->text, text);
    if (ts == NULL) {
        now_text(slot->time, sizeof slot->time);
    } else {
        flat_copy(slot->time, sizeof slot->time, ts);
    }

    g_next = (g_next + 1) % HA_LOG_CAP;
    if (g_count < HA_LOG_CAP) {
        g_count++;
    }
}

/* 调用方必须持锁。 */
static void file_append(const ha_log_entry_t *entry)
{
    FILE *fp = fopen(g_path, "a");

    if (fp == NULL) {
        return;
    }
    fprintf(fp, "%s\t%s\t%s\n", entry->time, entry->kind, entry->text);
    fclose(fp);
}

void ha_log_init(void)
{
    char line[512];
    FILE *fp;

    log_path(g_path, sizeof g_path);

    fp = fopen(g_path, "r");
    if (fp != NULL) {
        while (fgets(line, sizeof line, fp) != NULL) {
            char ts[32];
            char kind[32];
            char *t1 = strchr(line, '\t');
            char *t2;

            if (t1 == NULL) {
                continue;
            }
            *t1 = '\0';
            t2 = strchr(t1 + 1, '\t');
            if (t2 == NULL) {
                continue;
            }
            *t2 = '\0';

            snprintf(ts, sizeof ts, "%s", line);
            snprintf(kind, sizeof kind, "%s", t1 + 1);

            pthread_mutex_lock(&g_lock);
            ring_push(kind, t2 + 1, ts);
            pthread_mutex_unlock(&g_lock);
        }
        fclose(fp);
    }

    /* 回写一次，把文件裁剪到内存环的大小，避免长期运行无限增长。 */
    pthread_mutex_lock(&g_lock);
    fp = fopen(g_path, "w");
    if (fp != NULL) {
        int oldest = (g_next - g_count + HA_LOG_CAP) % HA_LOG_CAP;
        int i;

        for (i = 0; i < g_count; i++) {
            const ha_log_entry_t *e = &g_ring[(oldest + i) % HA_LOG_CAP];

            fprintf(fp, "%s\t%s\t%s\n", e->time, e->kind, e->text);
        }
        fclose(fp);
    }
    pthread_mutex_unlock(&g_lock);
}

void ha_log_add(const char *kind, const char *fmt, ...)
{
    char text[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_lock);
    ring_push(kind, text, NULL);
    file_append(&g_ring[(g_next + HA_LOG_CAP - 1) % HA_LOG_CAP]);
    pthread_mutex_unlock(&g_lock);
}

int ha_log_json(char *buf, size_t size, int limit)
{
    size_t used = 0;
    int emitted = 0;
    int total;
    int i;

    if (limit <= 0 || limit > HA_LOG_CAP) {
        limit = HA_LOG_CAP;
    }

    pthread_mutex_lock(&g_lock);
    total = g_count;

    if (used + 2 >= size) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    buf[used++] = '[';

    for (i = 0; i < total && emitted < limit; i++) {
        int idx = (g_next - 1 - i + 2 * HA_LOG_CAP) % HA_LOG_CAP;
        char esc_time[64];
        char esc_kind[64];
        char esc_text[768];
        int n;

        json_escape(g_ring[idx].time, esc_time, sizeof esc_time);
        json_escape(g_ring[idx].kind, esc_kind, sizeof esc_kind);
        json_escape(g_ring[idx].text, esc_text, sizeof esc_text);

        n = snprintf(buf + used, size - used,
                     "%s{\"time\":\"%s\",\"kind\":\"%s\",\"text\":\"%s\"}",
                     emitted == 0 ? "" : ",", esc_time, esc_kind, esc_text);
        if (n < 0 || (size_t)n >= size - used) {
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
        used += (size_t)n;
        emitted++;
    }

    buf[used++] = ']';
    buf[used] = '\0';
    pthread_mutex_unlock(&g_lock);
    return (int)used;
}
