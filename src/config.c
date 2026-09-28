#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HA_CFG_FILE "config.conf"

void ha_config_defaults(ha_config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->enabled = 1;
}

static void config_path(char *buf, size_t size)
{
    const char *dir = getenv("AINICE_PLUGIN_DATA");

    if (dir == NULL || *dir == '\0') {
        dir = ".";
    }
    snprintf(buf, size, "%s/" HA_CFG_FILE, dir);
}

static void copy_trim(char *dst, size_t size, const char *src)
{
    size_t len;

    while (*src == ' ' || *src == '\t') {
        src++;
    }
    len = strlen(src);
    while (len > 0 && (src[len - 1] == '\r' || src[len - 1] == '\n' ||
                       src[len - 1] == ' ' || src[len - 1] == '\t')) {
        len--;
    }
    if (len >= size) {
        len = size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* 值里出现换行会破坏 key=value 结构，写入前一律拒绝。 */
static int has_control(const char *value)
{
    for (; *value != '\0'; value++) {
        if (*value == '\n' || *value == '\r') {
            return 1;
        }
    }
    return 0;
}

static void strip_trailing_slash(char *url)
{
    size_t len = strlen(url);

    while (len > 0 && url[len - 1] == '/') {
        url[--len] = '\0';
    }
}

int ha_config_url_valid(const char *url)
{
    const char *host;

    if (url[0] == '\0') {
        return 1;
    }
    if (strncmp(url, "http://", 7) == 0) {
        host = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        host = url + 8;
    } else {
        return 0;
    }

    /* 协议头后面剩下的必须含主机名；只有斜杠或空白的一律拒绝。 */
    for (; *host != '\0'; host++) {
        if (*host != '/' && *host != ' ' && *host != '\t') {
            return 1;
        }
    }
    return 0;
}

int ha_config_webhook_id_valid(const char *id)
{
    for (; *id != '\0'; id++) {
        unsigned char c = (unsigned char)*id;

        if (!(isalnum(c) || c == '_' || c == '-' || c == '.' || c == '~')) {
            return 0;
        }
    }
    return 1;
}

void ha_config_load(ha_config_t *cfg)
{
    char path[512];
    char line[1024];
    FILE *fp;

    ha_config_defaults(cfg);
    config_path(path, sizeof path);

    fp = fopen(path, "r");
    if (fp == NULL) {
        return;
    }

    while (fgets(line, sizeof line, fp) != NULL) {
        char *eq = strchr(line, '=');
        const char *value;

        if (eq == NULL || line[0] == '#') {
            continue;
        }
        *eq = '\0';
        value = eq + 1;

        if (strcmp(line, "enabled") == 0) {
            cfg->enabled = (atoi(value) != 0);
        } else if (strcmp(line, "ha_url") == 0) {
            copy_trim(cfg->ha_url, sizeof cfg->ha_url, value);
        } else if (strcmp(line, "webhook_id") == 0) {
            copy_trim(cfg->webhook_id, sizeof cfg->webhook_id, value);
        } else if (strcmp(line, "secret") == 0) {
            copy_trim(cfg->secret, sizeof cfg->secret, value);
        }
    }
    fclose(fp);

    /* 文件里可能残留早前存下的坏值（例如只有协议头的地址、含非法字符的
       webhook_id），一律判为未配置，避免带着必然失败的目标去投递。 */
    if (!ha_config_url_valid(cfg->ha_url)) {
        cfg->ha_url[0] = '\0';
    }
    if (!ha_config_webhook_id_valid(cfg->webhook_id)) {
        cfg->webhook_id[0] = '\0';
    }
    strip_trailing_slash(cfg->ha_url);
}

int ha_config_save(const ha_config_t *cfg)
{
    ha_config_t norm = *cfg;
    char path[512];
    char tmp[560];
    FILE *fp;

    if (has_control(norm.ha_url) || has_control(norm.webhook_id) || has_control(norm.secret)) {
        return -1;
    }
    if (!ha_config_url_valid(norm.ha_url)) {
        return -1;
    }
    if (!ha_config_webhook_id_valid(norm.webhook_id)) {
        return -1;
    }
    strip_trailing_slash(norm.ha_url);

    config_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    fp = fopen(tmp, "w");
    if (fp == NULL) {
        return -1;
    }

    fprintf(fp, "enabled=%d\n", norm.enabled ? 1 : 0);
    fprintf(fp, "ha_url=%s\n", norm.ha_url);
    fprintf(fp, "webhook_id=%s\n", norm.webhook_id);
    fprintf(fp, "secret=%s\n", norm.secret);

    if (fclose(fp) != 0) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

void ha_config_target_url(const ha_config_t *cfg, char *buf, size_t size)
{
    if (cfg->ha_url[0] == '\0' || cfg->webhook_id[0] == '\0') {
        snprintf(buf, size, "%s", "");
        return;
    }
    snprintf(buf, size, "%s/api/webhook/%s", cfg->ha_url, cfg->webhook_id);
}
