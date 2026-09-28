#include "presence.h"

#include "dispatch.h"
#include "eventlog.h"
#include "json.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * 平台 C SDK 的 presence 订阅入口。
 *
 * 开发者文档只给出了 Python 侧的 event.subscribe_presence()，没有列出 C 符号名，
 * 因此这里按 Python 命名习惯声明为弱符号：
 *   - 板端 libainice.so 若导出同名符号，自动触发直接生效；
 *   - 若导出的是别的名字（例如 ainice_event_presence_subscribe），
 *     弱符号解析为 NULL，本文件只记一条日志，Bridge 手动触发不受影响。
 *
 * 对齐方式：在设备上执行
 *   nm -D --defined-only /usr/lib/libainice.so | grep -i presence
 * 查到真实符号名后，替换下面的名字与回调签名即可；改动只涉及本文件。
 */
extern int ainice_event_subscribe_presence(int (*cb)(const char *snapshot_json, void *userdata),
                                           void *userdata) __attribute__((weak));

#define HA_PRESENCE_ZONE_CAP 16
#define HA_PRESENCE_FIRST_RETRY_S 5
#define HA_PRESENCE_MAX_RETRY_S 60

typedef struct {
    char name[HA_ZONE_MAX];
    int present;
} zone_state_t;

/* 只由 SDK 回调线程访问，无需加锁。 */
static int g_have_baseline;
static int g_global_present;
static zone_state_t g_zones[HA_PRESENCE_ZONE_CAP];
static int g_zone_count;

static void remember_zones(cJSON *zones_root)
{
    int n;
    int i;

    g_zone_count = 0;
    if (zones_root == NULL) {
        return;
    }

    n = cJSON_GetArraySize(zones_root);
    for (i = 0; i < n && g_zone_count < HA_PRESENCE_ZONE_CAP; i++) {
        cJSON *item = cJSON_GetArrayItem(zones_root, i);
        cJSON *name;
        cJSON *present;
        zone_state_t *slot;

        if (!cJSON_IsObject(item)) {
            continue;
        }
        name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsString(name) || name->valuestring == NULL) {
            name = cJSON_GetObjectItemCaseSensitive(item, "id");
        }
        if (!cJSON_IsString(name) || name->valuestring == NULL) {
            continue;
        }
        present = cJSON_GetObjectItemCaseSensitive(item, "present");

        slot = &g_zones[g_zone_count++];
        snprintf(slot->name, sizeof slot->name, "%s", name->valuestring);
        slot->present = cJSON_IsTrue(present) ? 1 : 0;
    }
}

/* 在快照列表里找同名关注区，找到返回 1 并写入 present。 */
static int lookup_zone(const zone_state_t *list, int count, const char *name, int *present)
{
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(list[i].name, name) == 0) {
            *present = list[i].present;
            return 1;
        }
    }
    return 0;
}

static void emit(const char *event, const char *zone, int has_present, int present)
{
    ha_event_t ev;

    memset(&ev, 0, sizeof ev);
    snprintf(ev.source, sizeof ev.source, "presence");
    snprintf(ev.event, sizeof ev.event, "%s", event);
    if (zone != NULL) {
        snprintf(ev.zone, sizeof ev.zone, "%s", zone);
    }
    ev.has_present = has_present;
    ev.present = present;

    if (ha_dispatch_enqueue(&ev) != 0) {
        return; /* enqueue 内部已记日志 */
    }
    ha_log_add("presence", "状态变化 %s%s%s",
               event, zone != NULL ? " zone=" : "", zone != NULL ? zone : "");
}

static int on_presence(const char *snapshot_json, void *userdata)
{
    cJSON *root;
    cJSON *global_present;
    cJSON *zones;
    zone_state_t prev[HA_PRESENCE_ZONE_CAP];
    int prev_count;
    int global_now;
    int i;

    (void)userdata;

    if (snapshot_json == NULL) {
        return 0;
    }
    root = cJSON_Parse(snapshot_json);
    if (root == NULL) {
        ha_log_add("presence", "快照解析失败，已忽略");
        return 0;
    }

    global_present = cJSON_GetObjectItemCaseSensitive(root, "global_present");
    global_now = cJSON_IsTrue(global_present) ? 1 : 0;
    zones = cJSON_GetObjectItemCaseSensitive(root, "zones");

    /* 第一帧只作为基线：避免插件启动瞬间把当前状态当成状态变化推给 HA。 */
    if (!g_have_baseline) {
        g_global_present = global_now;
        remember_zones(zones);
        g_have_baseline = 1;
        ha_log_add("presence", "已建立状态基线：全局%s，关注区 %d 个",
                   global_now ? "有人" : "无人", g_zone_count);
        cJSON_Delete(root);
        return 0;
    }

    if (global_now != g_global_present) {
        g_global_present = global_now;
        emit(global_now ? "global_present" : "global_absent", NULL, 0, global_now);
    }

    /* 先比对旧快照，再整体换成新快照。 */
    prev_count = g_zone_count;
    memcpy(prev, g_zones, sizeof prev);
    remember_zones(zones);

    for (i = 0; i < g_zone_count; i++) {
        int was;

        /* 新出现的关注区不算状态变化。 */
        if (!lookup_zone(prev, prev_count, g_zones[i].name, &was)) {
            continue;
        }
        if (was != g_zones[i].present) {
            emit(g_zones[i].present ? "zone_present" : "zone_absent",
                 g_zones[i].name, 1, g_zones[i].present);
        }
    }

    cJSON_Delete(root);
    return 0;
}

static void *presence_main(void *arg)
{
    int delay = HA_PRESENCE_FIRST_RETRY_S;

    (void)arg;

    if (ainice_event_subscribe_presence == NULL) {
        ha_log_add("presence",
                   "未找到 presence 订阅符号，自动触发停用；请按 src/presence.c 顶部说明对齐符号名");
        return NULL;
    }

    for (;;) {
        int rc = ainice_event_subscribe_presence(on_presence, NULL);

        ha_log_add("presence", "订阅退出(rc=%d)，%d 秒后重连", rc, delay);
        sleep((unsigned)delay);
        if (delay < HA_PRESENCE_MAX_RETRY_S) {
            delay *= 2;
            if (delay > HA_PRESENCE_MAX_RETRY_S) {
                delay = HA_PRESENCE_MAX_RETRY_S;
            }
        }
    }
    return NULL;
}

void ha_presence_start(void)
{
    pthread_t tid;

    if (pthread_create(&tid, NULL, presence_main, NULL) != 0) {
        ha_log_add("presence", "启动订阅线程失败，自动触发停用");
        return;
    }
    pthread_detach(tid);
}

int ha_presence_available(void)
{
    return ainice_event_subscribe_presence != NULL;
}
