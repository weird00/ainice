#ifndef HA_WEBHOOK_DISPATCH_H
#define HA_WEBHOOK_DISPATCH_H

#include "config.h"
#include "http.h"

#define HA_ZONE_MAX 64
#define HA_EVENT_MAX 32

/* 一条待投递事件。source 同时作为日志类别。 */
typedef struct {
    char source[16]; /* presence / manual / test */
    char event[HA_EVENT_MAX];
    char zone[HA_ZONE_MAX];
    int has_present;
    int present;
} ha_event_t;

typedef struct {
    int enabled;
    int configured; /* ha_url 与 webhook_id 都已填 */
    char target_url[HA_TARGET_MAX];
    int queued;
    int sent_total;
    int fail_total;
    int last_status; /* ha_http_status_t */
    char last_detail[256];
    char last_time[20];
} ha_state_t;

void ha_dispatch_init(void);
void ha_dispatch_shutdown(void);

void ha_dispatch_config_snapshot(ha_config_t *out);

/* 持久化并切换运行态配置；写盘失败返回 -1，内存配置保持不变。 */
int ha_dispatch_config_apply(const ha_config_t *in);

/* 自动触发：入队后立即返回，由后台线程投递并退避重试。队列满返回 -1。 */
int ha_dispatch_enqueue(const ha_event_t *ev);

/* 手动触发：同步投递一次，不重试，直接返回结果。 */
ha_http_result_t ha_dispatch_send_now(const ha_event_t *ev);

void ha_dispatch_state(ha_state_t *out);

#endif
