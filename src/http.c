#include "http.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define HA_HTTP_CONNECT_TIMEOUT_MS 3000
#define HA_HTTP_RESP_MAX 512

struct resp_buf {
    char data[HA_HTTP_RESP_MAX];
    size_t len;
};

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;

static void curl_setup(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

/* 响应体和 curl 错误串会进日志，先剔掉换行等控制字符。 */
static void copy_flat(char *dst, size_t size, const char *src)
{
    size_t di = 0;

    if (src == NULL) {
        src = "";
    }
    while (*src != '\0' && di + 1 < size) {
        unsigned char c = (unsigned char)*src++;

        dst[di++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    dst[di] = '\0';
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct resp_buf *rb = (struct resp_buf *)userdata;
    size_t n = size * nmemb;

    if (rb->len + 1 < sizeof rb->data) {
        size_t room = sizeof rb->data - 1 - rb->len;

        if (n > room) {
            n = room;
        }
        memcpy(rb->data + rb->len, ptr, n);
        rb->len += n;
    }
    /* 必须返回收到的全部字节，否则 libcurl 会当成写失败。 */
    return size * nmemb;
}

ha_http_result_t ha_http_post_json(const char *url, const char *body, int timeout_ms)
{
    ha_http_result_t res;
    struct resp_buf rb;
    struct curl_slist *headers = NULL;
    CURL *curl;
    CURLcode rc;

    memset(&res, 0, sizeof res);
    res.http_code = -1;
    memset(&rb, 0, sizeof rb);

    if (url == NULL || url[0] == '\0' || body == NULL) {
        res.status = HA_HTTP_ERR_CONFIG;
        copy_flat(res.detail, sizeof res.detail, "目标地址或请求体为空");
        return res;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 5000;
    }

    pthread_once(&g_curl_once, curl_setup);

    curl = curl_easy_init();
    if (curl == NULL) {
        res.status = HA_HTTP_ERR_OTHER;
        copy_flat(res.detail, sizeof res.detail, "curl_easy_init 失败");
        return res;
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: */*");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)HA_HTTP_CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ainice-ha-webhook/1.0");

    rc = curl_easy_perform(curl);

    if (rc == CURLE_OK) {
        long code = 0;

        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        res.http_code = (int)code;
        rb.data[rb.len] = '\0';
        if (code >= 200 && code < 300) {
            res.status = HA_HTTP_OK;
            snprintf(res.detail, sizeof res.detail, "HTTP %d %s", (int)code, rb.data);
        } else {
            res.status = HA_HTTP_ERR_STATUS;
            snprintf(res.detail, sizeof res.detail, "HTTP %d %s", (int)code, rb.data);
        }
        copy_flat(res.detail, sizeof res.detail, res.detail);
    } else if (rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST ||
               rc == CURLE_OPERATION_TIMEDOUT || rc == CURLE_GOT_NOTHING ||
               rc == CURLE_SEND_ERROR || rc == CURLE_RECV_ERROR) {
        res.status = HA_HTTP_ERR_CONNECT;
        copy_flat(res.detail, sizeof res.detail, curl_easy_strerror(rc));
    } else {
        res.status = HA_HTTP_ERR_OTHER;
        copy_flat(res.detail, sizeof res.detail, curl_easy_strerror(rc));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}

int ha_http_retryable(ha_http_status_t status)
{
    return status == HA_HTTP_ERR_CONNECT;
}

const char *ha_http_status_name(ha_http_status_t status)
{
    switch (status) {
    case HA_HTTP_OK:
        return "ok";
    case HA_HTTP_ERR_CONFIG:
        return "config";
    case HA_HTTP_ERR_CONNECT:
        return "connect";
    case HA_HTTP_ERR_STATUS:
        return "http";
    default:
        return "other";
    }
}
