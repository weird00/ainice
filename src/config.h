#ifndef HA_WEBHOOK_CONFIG_H
#define HA_WEBHOOK_CONFIG_H

#include <stddef.h>

#define HA_CFG_MAX_URL 256
#define HA_CFG_MAX_ID 128
#define HA_CFG_MAX_SECRET 128

/* 拼好的目标地址缓冲区尺寸：ha_url + "/api/webhook/" + webhook_id */
#define HA_TARGET_MAX (HA_CFG_MAX_URL + HA_CFG_MAX_ID + 32)

typedef struct {
    int enabled;                     /* 总开关，0 = 自动触发只记录不发送 */
    char ha_url[HA_CFG_MAX_URL];     /* 例如 http://192.168.1.10:8123，不含尾部斜杠 */
    char webhook_id[HA_CFG_MAX_ID];  /* HA 自动化里 webhook 触发器的 webhook_id */
    char secret[HA_CFG_MAX_SECRET];  /* 可选共享密钥，作为 payload.token 发送，由 HA 侧条件校验 */
} ha_config_t;

void ha_config_defaults(ha_config_t *cfg);

/* 读取 $AINICE_PLUGIN_DATA/config.conf；文件不存在或字段缺失时保留默认值。
   返回后 ha_url 已去掉尾部斜杠；文件里的地址或 webhook_id 若不合法
   （例如只剩协议头的地址、含 URL 不安全字符的 id），按未配置处理并清空。 */
void ha_config_load(ha_config_t *cfg);

/* 判断 HA 地址是否可用：留空视为合法（表示暂不投递）；
   非空时必须是 http:// 或 https:// 且协议头之后确有主机名，
   "http://"、"http:///" 这类只有协议头的地址一律不合法。 */
int ha_config_url_valid(const char *url);

/* 判断 webhook_id 是否可用：留空视为合法（表示暂不投递）；
   非空时只允许字母、数字与 _ - . ~，保证能直接拼进 URL 路径。 */
int ha_config_webhook_id_valid(const char *id);

/* 校验并原子写入 config.conf；成功返回 0，校验失败或写盘失败返回 -1。 */
int ha_config_save(const ha_config_t *cfg);

/* 生成实际投递地址；未配置完整时写入空串。 */
void ha_config_target_url(const ha_config_t *cfg, char *buf, size_t size);

#endif
