#include "dispatch.h"

#include "eventlog.h"
#include "json.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HA_QUEUE_CAP 32
#define HA_RETRY_MAX 3
#define HA_SEND_TIMEOUT_MS 5000
#define HA_BACKOFF_BASE_MS 2000

typedef struct {
    ha_event_t ev;
    int attempt;
} q_item_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_worker;
static int g_worker_running;
static int g_stop;

static ha_config_t g_cfg;
static q_item_t g_queue[HA_QUEUE_CAP];
static int g_q_head;
static int g_q_count;

static int g_sent_total;
static int g_fail_total;
static ha_http_result_t g_last;
static char g_last_time[20];
static char g_device[64] = "ainice";

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

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static char *build_payload(const ha_event_t *ev, const char *secret)
{
    cJSON *root = cJSON_CreateObject();
    char *text;

    if (root == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "source", ev->source);
    cJSON_AddStringToObject(root, "event", ev->event);
    cJSON_AddStringToObject(root, "device", g_device);
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));
    if (ev->zone[0] != '\0') {
        cJSON_AddStringToObject(root, "zone", ev->zone);
    }
    if (ev->has_present) {
        cJSON_AddBoolToObject(root, "present", ev->present ? 1 : 0);
    }
    /* HA 侧用 trigger.json.token 做条件校验，未配置时不下发该字段。 */
    if (secret != NULL && secret[0] != '\0') {
        cJSON_AddStringToObject(root, "token", secret);
    }

    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

/* 投递一条事件。cfg 为调用时的配置副本，避免持锁做网络 IO。 */
static ha_http_result_t send_event(const ha_event_t *ev, const ha_config_t *cfg, int timeout_ms)
{
    ha_http_result_t res;
    char url[HA_TARGET_MAX];
    char *body;
    const char *kind = ev->source[0] != '\0' ? ev->source : "auto";

    ha_config_target_url(cfg, url, sizeof url);
    if (url[0] == '\0') {
        memset(&res, 0, sizeof res);
        res.status = HA_HTTP_ERR_CONFIG;
        res.http_code = -1;
        snprintf(res.detail, sizeof res.detail, "未配置 HA 地址或 webhook_id");
        ha_log_add(kind, "跳过 event=%s：%s", ev->event, res.detail);
        return res;
    }

    body = build_payload(ev, cfg->secret);
    if (body == NULL) {
        memset(&res, 0, sizeof res);
        res.status = HA_HTTP_ERR_OTHER;
        res.http_code = -1;
        snprintf(res.detail, sizeof res.detail, "构造请求体失败");
        ha_log_add(kind, "跳过 event=%s：%s", ev->event, res.detail);
        return res;
    }

    res = ha_http_post_json(url, body, timeout_ms);
    free(body);

    if (res.status == HA_HTTP_OK) {
        ha_log_add(kind, "已投递 event=%s%s%s -> HA",
                   ev->event,
                   ev->zone[0] != '\0' ? " zone=" : "",
                   ev->zone[0] != '\0' ? ev->zone : "");
    } else {
        ha_log_add(kind, "投递失败(%s) event=%s：%s",
                   ha_http_status_name(res.status), ev->event, res.detail);
    }
    return res;
}

static void record_result(const ha_http_result_t *res)
{
    pthread_mutex_lock(&g_lock);
    if (res->status == HA_HTTP_OK) {
        g_sent_total++;
    } else {
        g_fail_total++;
    }
    g_last = *res;
    now_text(g_last_time, sizeof g_last_time);
    pthread_mutex_unlock(&g_lock);
}

static void *worker_main(void *arg)
{
    (void)arg;

    for (;;) {
        q_item_t item;
        ha_config_t cfg;
        ha_http_result_t res;

        pthread_mutex_lock(&g_lock);
        while (g_q_count == 0 && !g_stop) {
            pthread_cond_wait(&g_cond, &g_lock);
        }
        if (g_stop && g_q_count == 0) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        item = g_queue[g_q_head];
        g_q_head = (g_q_head + 1) % HA_QUEUE_CAP;
        g_q_count--;
        cfg = g_cfg;
        pthread_mutex_unlock(&g_lock);

        res = send_event(&item.ev, &cfg, HA_SEND_TIMEOUT_MS);
        record_result(&res);

        if (res.status != HA_HTTP_OK && ha_http_retryable(res.status) &&
            item.attempt + 1 < HA_RETRY_MAX) {
            sleep_ms(HA_BACKOFF_BASE_MS << item.attempt);
            item.attempt++;

            pthread_mutex_lock(&g_lock);
            if (g_stop) {
                pthread_mutex_unlock(&g_lock);
                break;
            }
            if (g_q_count < HA_QUEUE_CAP) {
                int tail = (g_q_head + g_q_count) % HA_QUEUE_CAP;

                g_queue[tail] = item;
                g_q_count++;
                pthread_cond_signal(&g_cond);
                pthread_mutex_unlock(&g_lock);
                continue;
            }
            pthread_mutex_unlock(&g_lock);
            ha_log_add("presence", "重试队列已满，丢弃 event=%s", item.ev.event);
        }
    }
    return NULL;
}

void ha_dispatch_init(void)
{
    ha_config_load(&g_cfg);

    if (gethostname(g_device, sizeof g_device - 1) != 0) {
        snprintf(g_device, sizeof g_device, "ainice");
    }
    g_device[sizeof g_device - 1] = '\0';

    if (pthread_create(&g_worker, NULL, worker_main, NULL) == 0) {
        g_worker_running = 1;
    }
}

void ha_dispatch_shutdown(void)
{
    if (!g_worker_running) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_stop = 1;
    g_q_count = 0;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_worker, NULL);
    g_worker_running = 0;
}

void ha_dispatch_config_snapshot(ha_config_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_cfg;
    pthread_mutex_unlock(&g_lock);
}

int ha_dispatch_config_apply(const ha_config_t *in)
{
    ha_config_t norm;

    if (ha_config_save(in) != 0) {
        return -1;
    }
    /* 回读一次，保证内存态与磁盘一致（含尾部斜杠等规范化）。 */
    ha_config_load(&norm);
    pthread_mutex_lock(&g_lock);
    g_cfg = norm;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int ha_dispatch_enqueue(const ha_event_t *ev)
{
    int rc = 0;

    pthread_mutex_lock(&g_lock);
    if (!g_cfg.enabled) {
        rc = -1;
    } else if (g_q_count >= HA_QUEUE_CAP) {
        rc = -1;
    } else {
        int tail = (g_q_head + g_q_count) % HA_QUEUE_CAP;

        memset(&g_queue[tail], 0, sizeof g_queue[tail]);
        g_queue[tail].ev = *ev;
        g_q_count++;
        pthread_cond_signal(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);

    if (rc != 0) {
        ha_log_add(ev->source[0] != '\0' ? ev->source : "auto",
                   "丢弃 event=%s：自动触发已关闭或队列已满", ev->event);
    }
    return rc;
}

ha_http_result_t ha_dispatch_send_now(const ha_event_t *ev)
{
    ha_config_t cfg;
    ha_http_result_t res;

    ha_dispatch_config_snapshot(&cfg);
    /* 手动触发不受总开关限制，便于配置完成前先验证链路。 */
    res = send_event(ev, &cfg, HA_SEND_TIMEOUT_MS);
    record_result(&res);
    return res;
}

void ha_dispatch_state(ha_state_t *out)
{
    pthread_mutex_lock(&g_lock);
    memset(out, 0, sizeof *out);
    out->enabled = g_cfg.enabled;
    ha_config_target_url(&g_cfg, out->target_url, sizeof out->target_url);
    out->configured = out->target_url[0] != '\0';
    out->queued = g_q_count;
    out->sent_total = g_sent_total;
    out->fail_total = g_fail_total;
    out->last_status = (int)g_last.status;
    snprintf(out->last_detail, sizeof out->last_detail, "%s", g_last.detail);
    snprintf(out->last_time, sizeof out->last_time, "%s", g_last_time);
    pthread_mutex_unlock(&g_lock);
}
