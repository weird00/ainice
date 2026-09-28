#ifndef HA_WEBHOOK_HTTP_H
#define HA_WEBHOOK_HTTP_H

#include <stddef.h>

typedef enum {
    HA_HTTP_OK = 0,      /* 2xx，HA 已接收 */
    HA_HTTP_ERR_CONFIG,  /* 地址或参数不合法，重试无意义 */
    HA_HTTP_ERR_CONNECT, /* 连接失败、域名解析失败或超时，可重试 */
    HA_HTTP_ERR_STATUS,  /* 已连上但返回非 2xx，例如 404 webhook 不存在 */
    HA_HTTP_ERR_OTHER
} ha_http_status_t;

typedef struct {
    ha_http_status_t status;
    int http_code;  /* 未收到响应时为 -1 */
    char detail[256];
} ha_http_result_t;

/* 对 url 做一次 POST，body 为 JSON 文本。
   失败一律通过返回值表达，不写 stderr、不退出进程。 */
ha_http_result_t ha_http_post_json(const char *url, const char *body, int timeout_ms);

/* 结果是否值得重试。 */
int ha_http_retryable(ha_http_status_t status);

const char *ha_http_status_name(ha_http_status_t status);

#endif
