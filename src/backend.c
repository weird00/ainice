#include <ainice/bridge.h>

#include "config.h"
#include "dispatch.h"
#include "eventlog.h"
#include "json.h"
#include "presence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HA_MODULE "ha-webhook"
#define HA_LOG_RESP_CAP 65536
#define HA_LOG_DEFAULT_LIMIT 30

static const char *str_param(cJSON *obj, const char *key)
{
    cJSON *item;

    if (obj == NULL) {
        return NULL;
    }
    item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return NULL;
    }
    return item->valuestring;
}

static void set_string(char *dst, size_t size, const char *value)
{
    snprintf(dst, size, "%s", value);
}

static cJSON *config_to_json(const ha_config_t *cfg)
{
    cJSON *obj = cJSON_CreateObject();
    char target[HA_TARGET_MAX];

    if (obj == NULL) {
        return NULL;
    }

    ha_config_target_url(cfg, target, sizeof target);
    cJSON_AddBoolToObject(obj, "enabled", cfg->enabled);
    cJSON_AddStringToObject(obj, "ha_url", cfg->ha_url);
    cJSON_AddStringToObject(obj, "webhook_id", cfg->webhook_id);
    cJSON_AddStringToObject(obj, "secret", cfg->secret);
    cJSON_AddStringToObject(obj, "target_url", target);
    return obj;
}

static int respond(ainice_bridge_response_t *response, cJSON *root)
{
    char *text;
    int rc;

    if (root == NULL) {
        return ainice_bridge_response_json(response, "{\"ok\":false,\"error\":\"内部错误\"}");
    }
    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return ainice_bridge_response_json(response, "{\"ok\":false,\"error\":\"序列化失败\"}");
    }
    rc = ainice_bridge_response_json(response, text);
    free(text);
    return rc;
}

static int respond_error(ainice_bridge_response_t *response, const char *message)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        return ainice_bridge_response_json(response, "{\"ok\":false}");
    }
    cJSON_AddBoolToObject(root, "ok", 0);
    cJSON_AddStringToObject(root, "error", message);
    return respond(response, root);
}

static int respond_ok_config(ainice_bridge_response_t *response, const ha_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *node = config_to_json(cfg);

    if (root == NULL || node == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(node);
        return respond_error(response, "内部错误");
    }
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddItemToObject(root, "config", node);
    return respond(response, root);
}

static int method_config_get(ainice_bridge_response_t *response)
{
    ha_config_t cfg;

    ha_dispatch_config_snapshot(&cfg);
    return respond_ok_config(response, &cfg);
}

static int method_config_save(ainice_bridge_response_t *response, cJSON *params)
{
    ha_config_t cfg;
    cJSON *enabled;
    const char *value;

    ha_dispatch_config_snapshot(&cfg);

    enabled = params != NULL ? cJSON_GetObjectItemCaseSensitive(params, "enabled") : NULL;
    if (enabled != NULL) {
        cfg.enabled = cJSON_IsTrue(enabled) ? 1 : 0;
    }
    if ((value = str_param(params, "ha_url")) != NULL) {
        set_string(cfg.ha_url, sizeof cfg.ha_url, value);
    }
    if ((value = str_param(params, "webhook_id")) != NULL) {
        set_string(cfg.webhook_id, sizeof cfg.webhook_id, value);
    }
    if ((value = str_param(params, "secret")) != NULL) {
        set_string(cfg.secret, sizeof cfg.secret, value);
    }

    if (!ha_config_url_valid(cfg.ha_url)) {
        return respond_error(response, "HA 地址需要形如 http://192.168.1.10:8123，不能只填 http://");
    }
    if (!ha_config_webhook_id_valid(cfg.webhook_id)) {
        return respond_error(response, "webhook_id 只能包含字母、数字与 _ - . ~");
    }
    if (ha_dispatch_config_apply(&cfg) != 0) {
        return respond_error(response, "配置写入失败");
    }

    ha_log_add("system", "配置已更新：enabled=%d url=%s webhook_id=%s",
               cfg.enabled, cfg.ha_url[0] != '\0' ? cfg.ha_url : "(空)",
               cfg.webhook_id[0] != '\0' ? cfg.webhook_id : "(空)");
    return method_config_get(response);
}

static int method_state_get(ainice_bridge_response_t *response)
{
    ha_state_t state;
    cJSON *root = cJSON_CreateObject();
    cJSON *last = cJSON_CreateObject();

    if (root == NULL || last == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(last);
        return respond_error(response, "内部错误");
    }

    ha_dispatch_state(&state);

    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddBoolToObject(root, "enabled", state.enabled);
    cJSON_AddBoolToObject(root, "configured", state.configured);
    cJSON_AddStringToObject(root, "target_url", state.target_url);
    cJSON_AddBoolToObject(root, "presence_available", ha_presence_available());
    cJSON_AddNumberToObject(root, "queued", state.queued);
    cJSON_AddNumberToObject(root, "sent_total", state.sent_total);
    cJSON_AddNumberToObject(root, "fail_total", state.fail_total);

    cJSON_AddStringToObject(last, "status",
                            ha_http_status_name((ha_http_status_t)state.last_status));
    cJSON_AddStringToObject(last, "detail", state.last_detail);
    cJSON_AddStringToObject(last, "time", state.last_time);
    cJSON_AddItemToObject(root, "last", last);

    return respond(response, root);
}

static int method_log_get(ainice_bridge_response_t *response, cJSON *params)
{
    cJSON *limit_item = params != NULL ? cJSON_GetObjectItemCaseSensitive(params, "limit") : NULL;
    int limit = HA_LOG_DEFAULT_LIMIT;
    char *items;
    char *text;
    int n;
    int rc;

    if (cJSON_IsNumber(limit_item)) {
        limit = (int)limit_item->valuedouble;
    }

    items = malloc(HA_LOG_RESP_CAP);
    if (items == NULL) {
        return respond_error(response, "内存不足");
    }
    n = ha_log_json(items, HA_LOG_RESP_CAP, limit);
    if (n < 0) {
        free(items);
        return respond_error(response, "日志缓冲不足");
    }

    text = malloc((size_t)n + 64);
    if (text == NULL) {
        free(items);
        return respond_error(response, "内存不足");
    }
    snprintf(text, (size_t)n + 64, "{\"ok\":true,\"items\":%s}", items);
    rc = ainice_bridge_response_json(response, text);
    free(text);
    free(items);
    return rc;
}

static ha_event_t event_from_params(cJSON *params, const char *default_source,
                                    const char *default_event)
{
    ha_event_t ev;
    const char *value;
    cJSON *present;

    memset(&ev, 0, sizeof ev);
    set_string(ev.source, sizeof ev.source, default_source);
    set_string(ev.event, sizeof ev.event, default_event);

    if ((value = str_param(params, "source")) != NULL && value[0] != '\0') {
        set_string(ev.source, sizeof ev.source, value);
    }
    if ((value = str_param(params, "event")) != NULL && value[0] != '\0') {
        set_string(ev.event, sizeof ev.event, value);
    }
    if ((value = str_param(params, "zone")) != NULL) {
        set_string(ev.zone, sizeof ev.zone, value);
    }
    present = params != NULL ? cJSON_GetObjectItemCaseSensitive(params, "present") : NULL;
    if (present != NULL && (cJSON_IsBool(present) || cJSON_IsNumber(present))) {
        ev.has_present = 1;
        ev.present = cJSON_IsTrue(present) ? 1 : 0;
    }
    return ev;
}

static int respond_send_result(ainice_bridge_response_t *response, const ha_http_result_t *res)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        return respond_error(response, "内部错误");
    }
    cJSON_AddBoolToObject(root, "ok", res->status == HA_HTTP_OK ? 1 : 0);
    cJSON_AddStringToObject(root, "status", ha_http_status_name(res->status));
    cJSON_AddNumberToObject(root, "http_code", res->http_code);
    cJSON_AddStringToObject(root, "detail", res->detail);
    return respond(response, root);
}

static int method_trigger_send(ainice_bridge_response_t *response, cJSON *params)
{
    ha_event_t ev = event_from_params(params, "manual", "manual");
    ha_http_result_t res = ha_dispatch_send_now(&ev);

    return respond_send_result(response, &res);
}

static int method_trigger_test(ainice_bridge_response_t *response)
{
    ha_event_t ev;
    ha_http_result_t res;

    memset(&ev, 0, sizeof ev);
    set_string(ev.source, sizeof ev.source, "test");
    set_string(ev.event, sizeof ev.event, "test");
    res = ha_dispatch_send_now(&ev);
    return respond_send_result(response, &res);
}

static int handle(const char *request_json, ainice_bridge_response_t *response, void *userdata)
{
    cJSON *req;
    cJSON *params;
    const char *method;
    int rc;

    (void)userdata;

    req = cJSON_Parse(request_json != NULL ? request_json : "");
    if (req == NULL) {
        return respond_error(response, "请求不是合法 JSON");
    }

    method = str_param(req, "method");
    if (method == NULL) {
        cJSON_Delete(req);
        return respond_error(response, "缺少 method");
    }
    params = cJSON_GetObjectItemCaseSensitive(req, "params");
    if (!cJSON_IsObject(params)) {
        params = NULL;
    }

    if (strcmp(method, "config.get") == 0) {
        rc = method_config_get(response);
    } else if (strcmp(method, "config.save") == 0) {
        rc = method_config_save(response, params);
    } else if (strcmp(method, "state.get") == 0) {
        rc = method_state_get(response);
    } else if (strcmp(method, "log.get") == 0) {
        rc = method_log_get(response, params);
    } else if (strcmp(method, "trigger.send") == 0) {
        rc = method_trigger_send(response, params);
    } else if (strcmp(method, "trigger.test") == 0) {
        rc = method_trigger_test(response);
    } else {
        rc = respond_error(response, "未知方法");
    }

    cJSON_Delete(req);
    return rc;
}

int main(void)
{
    ha_log_init();
    ha_dispatch_init();
    ha_presence_start();
    ha_log_add("system", "插件启动，presence 订阅%s",
               ha_presence_available() ? "可用" : "不可用");

    return ainice_bridge_serve(HA_MODULE, handle, NULL);
}
