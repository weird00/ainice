#ifndef HA_WEBHOOK_PRESENCE_H
#define HA_WEBHOOK_PRESENCE_H

/* 订阅平台 presence 快照，把有人/无人变化转成 HA Webhook 事件。
   订阅符号不可用时只记录一条日志，Bridge 手动触发仍然可用。 */
void ha_presence_start(void);

/* 板端 SDK 是否提供 presence 订阅符号。 */
int ha_presence_available(void);

#endif
