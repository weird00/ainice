#ifndef HA_WEBHOOK_EVENTLOG_H
#define HA_WEBHOOK_EVENTLOG_H

#include <stddef.h>

#define HA_LOG_CAP 64

typedef struct {
    char time[20];  /* YYYY-MM-DD HH:MM:SS */
    char kind[16];  /* auto / manual / test */
    char text[224];
} ha_log_entry_t;

/* 从 $AINICE_PLUGIN_DATA/events.log 恢复最近 HA_LOG_CAP 条，并把文件裁剪到同样规模。 */
void ha_log_init(void);

/* 追加一条记录，同时落盘。fmt 不接受换行。 */
void ha_log_add(const char *kind, const char *fmt, ...);

/* 输出最近 limit 条（新到旧）的 JSON 数组；缓冲不足返回 -1。 */
int ha_log_json(char *buf, size_t size, int limit);

#endif
