#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""AInice 用户插件：Home Assistant Webhook 触发。

按插件契约实现：
- `<id>.app` 是插件进程入口，由常驻进程 ainice-video 按启用状态拉起；
- 配置、凭据和日志只写入 AINICE_PLUGIN_DATA（物理路径 /data/plugins/<id>）；
- 状态变化只通过 ainice.event 订阅，页面调用只通过 ainice.bridge 注册模块；
- 不连接平台内部 IPC，也不直接读写平台配置文件。

HA 地址与共享密钥只配置一次，由所有触发器复用；每个触发器各自带一个 webhook_id，
并各自指定监听范围（global 表示全局，或某个关注区 Zone-A…）与状态（有人 / 无人 / 两者），
两者都命中才投递，需要更多通路就再加一条触发器。

投递目标是 HA 的 Webhook 触发器：POST http://<ha-host>:8123/api/webhook/<webhook_id>，
webhook_id 本身即密钥，无需认证头；共享密钥作为请求体里的 token 字段下发，
由 HA 自动化侧的条件自行校验。
"""

from __future__ import annotations

import json
import os
import queue
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
from collections import deque

MODULE_NAME = "ha-webhook"
PLUGIN_DATA = os.environ.get("AINICE_PLUGIN_DATA") or "/data/plugins/ha-webhook"
CONFIG_PATH = os.path.join(PLUGIN_DATA, "config.json")
LOG_PATH = os.path.join(PLUGIN_DATA, "events.log")
LOG_LIMIT = 64
LOG_DEFAULT_LIMIT = 30
LOG_MAX_BYTES = 512 * 1024

HTTP_TIMEOUT = 5
HTTP_RESP_MAX = 512
USER_AGENT = "ainice-ha-webhook/1.0"

QUEUE_CAP = 32
RETRY_MAX = 3
BACKOFF_BASE_S = 2

# 触发器条数上限：够覆盖每个关注区的有人/无人，又能挡住页面侧的误操作。
TRIGGER_LIMIT = 32

STATUS_OK = "ok"
STATUS_CONFIG = "config"
STATUS_CONNECT = "connect"
STATUS_HTTP = "http"
STATUS_OTHER = "other"
RETRYABLE_STATUS = (STATUS_CONNECT,)

URL_INVALID_TEXT = "HA 地址需要形如 http://192.168.1.10:8123，不能只填 http://"
WEBHOOK_INVALID_TEXT = "webhook_id 只能包含字母、数字与 _ - . ~"
WEBHOOK_REQUIRED_TEXT = "每条触发器都要填写 Webhook ID"
TRIGGER_EVENT_INVALID_TEXT = "触发状态只能是有人、无人或两者"
SCOPE_INVALID_TEXT = "区域标识不能为空，也不能包含换行或制表符"
TRIGGER_LIMIT_TEXT = "触发器最多 %d 条" % TRIGGER_LIMIT
STORE_FAILED_TEXT = "配置写入失败"
NOT_CONFIGURED_TEXT = "没有可用的触发器"

WEBHOOK_ID_CHARS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.~")

# 触发器：scope 为 global 或某个关注区标识，event 为 present / absent / both。
SCOPE_GLOBAL = "global"
TRIGGER_EVENTS = ("present", "absent", "both")

_log_lock = threading.Lock()
_log_ring = deque(maxlen=LOG_LIMIT)

_config_lock = threading.Lock()
_config = {"enabled": True, "ha_url": "", "secret": "", "triggers": []}

_state_lock = threading.Lock()
_sent_total = 0
_fail_total = 0
_last_result = {"status": STATUS_OTHER, "http_code": -1, "detail": "", "time": ""}

_last_global_present = None
_zone_present = {}
_known_zones = []
_baseline_ready = False
_zone_format_warned = False

_send_queue = queue.Queue()
_device_name = "ainice"


# --------------------------------------------------------------------------- #
# 日志：内存环 + TSV 追加，条目形态与页面 log.get 的 items 一致
# --------------------------------------------------------------------------- #

def _now_text():
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())


def _flat(text):
    """只保留可打印字符，保证一行一条日志、TSV 结构不被破坏。"""
    chars = []
    for char in text:
        code = ord(char)
        chars.append(" " if code < 0x20 or code == 0x7F else char)
    return "".join(chars)


def log(kind, message):
    entry = {
        "time": _now_text(),
        "kind": _flat(str(kind)),
        "text": _flat(str(message)),
    }
    with _log_lock:
        _log_ring.append(entry)
        try:
            with open(LOG_PATH, "a", encoding="utf-8") as handle:
                handle.write("%s\t%s\t%s\n" % (entry["time"], entry["kind"], entry["text"]))
        except OSError:
            pass
    print("%s %s %s" % (entry["time"], entry["kind"], entry["text"]), file=sys.stderr, flush=True)


def _prepare_log_file():
    """日志文件超过上限时只保留尾部，避免长期运行撑大用户分区。"""
    try:
        if os.path.getsize(LOG_PATH) <= LOG_MAX_BYTES:
            return
        with open(LOG_PATH, "r", encoding="utf-8", errors="replace") as handle:
            tail = list(deque(handle, maxlen=LOG_LIMIT))
        with open(LOG_PATH, "w", encoding="utf-8") as handle:
            handle.writelines(tail)
    except OSError:
        pass


def _preload_log():
    try:
        with open(LOG_PATH, "r", encoding="utf-8", errors="replace") as handle:
            for line in deque(handle, maxlen=LOG_LIMIT):
                parts = line.rstrip("\n").split("\t", 2)
                if len(parts) != 3:
                    continue
                _log_ring.append({"time": parts[0], "kind": parts[1], "text": parts[2]})
    except OSError:
        pass


# --------------------------------------------------------------------------- #
# 配置
# --------------------------------------------------------------------------- #

def _default_config():
    return {"enabled": True, "ha_url": "", "secret": "", "triggers": []}


def _has_control(value):
    return "\n" in value or "\r" in value or "\t" in value


def _url_valid(url):
    if not url:
        return True
    if url.startswith("http://"):
        host = url[7:]
    elif url.startswith("https://"):
        host = url[8:]
    else:
        return False
    # 协议头后面必须含主机名，只有斜杠或空白的一律拒绝。
    return any(char not in "/ \t" for char in host)


def _webhook_id_valid(value):
    return all(char in WEBHOOK_ID_CHARS for char in value)


def _scope_valid(scope):
    """区域标识由插件自己写入（global 或 Zone-A），只拦必然破坏存储的值。"""
    return bool(scope) and len(scope) <= 64 and not _has_control(scope)


def _normalize_trigger(raw, fallback_id, strict):
    """把一条触发器收敛成固定 schema。

    strict 为真（页面保存）时保留非法的 event 字面值，交给 _triggers_error 报错；
    为假（读盘）时按缺省值兜底，避免坏值让触发器永远匹配不上。
    """
    if not isinstance(raw, dict):
        return None

    webhook_id = raw.get("webhook_id")
    webhook_id = webhook_id.strip() if isinstance(webhook_id, str) else ""
    # strict 为真（页面保存）时留着非法字面值，好让 _triggers_error 报出具体原因；
    # 保存会因为这条错误整体失败，非法值不会被写进配置文件。
    if not _webhook_id_valid(webhook_id) and not strict:
        webhook_id = ""

    scope = raw.get("scope")
    scope = scope.strip() if isinstance(scope, str) else ""
    if not scope:
        scope = SCOPE_GLOBAL

    event = raw.get("event")
    event = event.strip().lower() if isinstance(event, str) else ""
    if not event:
        event = "both"
    elif event not in TRIGGER_EVENTS and not strict:
        event = "both"

    trigger_id = raw.get("id")
    trigger_id = trigger_id.strip() if isinstance(trigger_id, str) else ""
    if not trigger_id or len(trigger_id) > 64 or _has_control(trigger_id):
        trigger_id = fallback_id

    return {"id": trigger_id, "webhook_id": webhook_id, "scope": scope, "event": event}


def _next_trigger_id(used):
    index = 1
    while "t%d" % index in used:
        index += 1
    return "t%d" % index


def _normalize_triggers(raw, strict):
    if not isinstance(raw, (list, tuple)):
        return []

    triggers = []
    used = set()
    for index, item in enumerate(raw):
        if len(triggers) >= TRIGGER_LIMIT:
            break
        trigger = _normalize_trigger(item, "t%d" % (index + 1), strict)
        if trigger is None:
            continue
        # id 是页面删除单条触发器的唯一依据，必须唯一。
        if trigger["id"] in used:
            trigger["id"] = _next_trigger_id(used)
        used.add(trigger["id"])
        triggers.append(trigger)
    return triggers


def _triggers_error(triggers):
    """返回第一条触发器的校验错误文案，全部合法时返回空串。"""
    for trigger in triggers:
        if not trigger["webhook_id"]:
            return WEBHOOK_REQUIRED_TEXT
        if not _webhook_id_valid(trigger["webhook_id"]):
            return WEBHOOK_INVALID_TEXT
        if trigger["event"] not in TRIGGER_EVENTS:
            return TRIGGER_EVENT_INVALID_TEXT
        if not _scope_valid(trigger["scope"]):
            return SCOPE_INVALID_TEXT
    return ""


def _normalize_config(raw):
    """收敛成固定 schema，并清掉早前可能存下的坏值。"""
    config = _default_config()
    if not isinstance(raw, dict):
        return config

    if isinstance(raw.get("enabled"), bool):
        config["enabled"] = raw["enabled"]
    for key in ("ha_url", "secret"):
        value = raw.get(key)
        config[key] = value.strip() if isinstance(value, str) else ""

    # 文件里可能残留只含协议头的地址，一律判为未配置，避免带着必然失败的目标去投递。
    if not _url_valid(config["ha_url"]):
        config["ha_url"] = ""
    config["ha_url"] = config["ha_url"].rstrip("/")

    config["triggers"] = _normalize_triggers(raw.get("triggers"), False)
    if not config["triggers"]:
        # 兼容单触发器时代的配置：顶层 webhook_id 视作「全局有人+无人」这一条触发器。
        legacy = raw.get("webhook_id")
        legacy = legacy.strip() if isinstance(legacy, str) else ""
        if legacy and _webhook_id_valid(legacy):
            config["triggers"] = [
                {"id": "t1", "webhook_id": legacy, "scope": SCOPE_GLOBAL, "event": "both"}
            ]
    return config


def _load_config():
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as handle:
            raw = json.load(handle)
    except FileNotFoundError:
        return _default_config()
    except (OSError, ValueError) as error:
        log("system", "读取配置失败，改用默认配置：%r" % (error,))
        return _default_config()
    return _normalize_config(raw)


def _store_config(config):
    os.makedirs(PLUGIN_DATA, exist_ok=True)
    temporary = CONFIG_PATH + ".tmp"
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(config, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    os.replace(temporary, CONFIG_PATH)


def _config_snapshot():
    with _config_lock:
        return json.loads(json.dumps(_config))


def set_config(config):
    global _config
    with _config_lock:
        _config = config


def _target_url(config, webhook_id):
    """HA 地址共用，webhook_id 由每条触发器各自提供。"""
    url = config["ha_url"]
    if not url or not webhook_id:
        return ""
    return "%s/api/webhook/%s" % (url, webhook_id)


def _config_view():
    return _config_snapshot()


# --------------------------------------------------------------------------- #
# 快照解析：按 zone_absence_holds 的位置对齐关注区
# --------------------------------------------------------------------------- #

def _attr(source, name, default=None):
    if isinstance(source, dict):
        return source.get(name, default)
    return getattr(source, name, default)


def _zone_key(index):
    """关注区名由位置推出：下标 0 → Zone-A。"""
    return "Zone-%s" % chr(ord("A") + index)


def _read_global(snapshot):
    """global_present 是布尔字段；取不到时返回 None，由调用方按未知处理。"""
    value = _attr(snapshot, "global_present")
    return value if isinstance(value, bool) else None


def _zone_index_of(token, count):
    """把 zones 集合里的元素归一成 0 基关注区下标，识别不了的返回 None。"""
    if isinstance(token, bool):
        return None
    if isinstance(token, int):
        return token if 0 <= token < count else None
    text = str(token).strip().lower()
    for prefix in ("zone-", "zone_", "zone ", "zone", "关注区"):
        if text.startswith(prefix):
            text = text[len(prefix):].strip()
            break
    if len(text) == 1 and "a" <= text <= "z":
        index = ord(text) - ord("a")
        return index if index < count else None
    if text.isdigit():
        index = int(text)
        return index if 0 <= index < count else None
    return None


def _present_zone_indices(snapshot, count):
    """返回当前"有人"的关注区下标集合。

    zones 是"有人"的关注区标识集合（无人时为空集），元素字面形态平台没有文档化，
    因此按序号、字母和 Zone-A 这类名字归一；识别不了时记一条日志便于排查。
    """
    global _zone_format_warned
    raw = _attr(snapshot, "zones")
    if isinstance(raw, dict):
        tokens = list(raw.keys())
    elif isinstance(raw, (set, frozenset, list, tuple)):
        tokens = list(raw)
    else:
        tokens = []
        if raw is not None and not _zone_format_warned:
            _zone_format_warned = True
            log("presence", "presence 快照的 zones 类型异常：%s" % type(raw).__name__)

    indices = set()
    unknown = []
    for token in tokens:
        index = _zone_index_of(token, count)
        if index is None:
            unknown.append(token)
        else:
            indices.add(index)
    if unknown and not _zone_format_warned:
        _zone_format_warned = True
        log("presence", "presence 快照的关注区标识无法识别：%r" % (unknown[:6],))
    return indices


def _extract_zones(snapshot):
    """返回 [{"key", "present"}]，按 zone_absence_holds 的位置对齐。

    平台契约：zone_absence_holds 是定长（等于关注区数量）元组，与关注区位置一一对应；
    zones 集合只列当前有人的关注区，所以"有人"要按位置去集合里查。
    """
    global _zone_format_warned
    holds = _attr(snapshot, "zone_absence_holds")
    if not isinstance(holds, (list, tuple)):
        if not _zone_format_warned:
            _zone_format_warned = True
            log("presence", "presence 快照缺少 zone_absence_holds，无法确定关注区数量：%r" % (holds,))
        return []

    count = len(holds)
    present = _present_zone_indices(snapshot, count)
    return [
        {"key": _zone_key(index), "present": index in present}
        for index in range(count)
    ]


# --------------------------------------------------------------------------- #
# 投递
# --------------------------------------------------------------------------- #

def _flat_exception(error):
    reason = getattr(error, "reason", None) or error
    return _flat(str(reason))


def _http_detail(code, raw):
    body = _flat(raw.decode("utf-8", "replace"))
    return ("HTTP %d %s" % (code, body)).rstrip()


def _event_text(event):
    """日志里事件的可读形态：zone_present zone=Zone-A。"""
    zone = event.get("zone") or ""
    return "%s%s%s" % (event.get("event") or "", " zone=" if zone else "", zone)


def _trigger_matches(event, trigger):
    """触发器的范围与状态都对上才算命中。"""
    if not trigger["webhook_id"]:
        return False
    if trigger["event"] != "both" and trigger["event"] != event.get("kind"):
        return False
    return trigger["scope"] == event.get("scope")


def _post_json(url, body, timeout):
    request = urllib.request.Request(
        url,
        data=body.encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Accept": "*/*",
            "User-Agent": USER_AGENT,
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            code = int(response.getcode() or 0)
            raw = response.read(HTTP_RESP_MAX)
    except urllib.error.HTTPError as error:
        code = int(error.code or 0)
        try:
            raw = error.read(HTTP_RESP_MAX)
        except OSError:
            raw = b""
        return {"status": STATUS_HTTP, "http_code": code, "detail": _http_detail(code, raw)}
    except (urllib.error.URLError, OSError) as error:
        # 连接被拒、域名解析失败、读写超时都归为 connect，可重试。
        return {"status": STATUS_CONNECT, "http_code": -1, "detail": _flat_exception(error)}

    if 200 <= code < 300:
        return {"status": STATUS_OK, "http_code": code, "detail": _http_detail(code, raw)}
    return {"status": STATUS_HTTP, "http_code": code, "detail": _http_detail(code, raw)}


def _build_payload(event, secret):
    payload = {
        "source": event.get("source") or "",
        "event": event.get("event") or "",
        "device": _device_name,
        "ts": int(time.time()),
    }
    zone = event.get("zone") or ""
    if zone:
        payload["zone"] = zone
    if event.get("has_present"):
        payload["present"] = bool(event.get("present"))
    # HA 侧用 trigger.json.token 做条件校验，未配置时不下发该字段。
    if secret:
        payload["token"] = secret
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def _send_event(event, trigger, timeout):
    """把一条事件投递给指定触发器，返回 {status, http_code, detail}。"""
    config = _config_snapshot()
    webhook_id = trigger["webhook_id"]
    url = _target_url(config, webhook_id)
    kind = event.get("source") or "auto"

    if not url:
        log(kind, "跳过 %s：%s" % (_event_text(event), NOT_CONFIGURED_TEXT))
        return {"status": STATUS_CONFIG, "http_code": -1, "detail": NOT_CONFIGURED_TEXT}

    result = _post_json(url, _build_payload(event, config["secret"]), timeout)
    if result["status"] == STATUS_OK:
        log(kind, "已投递 %s -> %s" % (_event_text(event), webhook_id))
    else:
        log(kind, "投递失败(%s) %s -> %s：%s"
            % (result["status"], _event_text(event), webhook_id, result["detail"]))
    return result


def _record_result(result):
    global _sent_total, _fail_total
    with _state_lock:
        if result["status"] == STATUS_OK:
            _sent_total += 1
        else:
            _fail_total += 1
        _last_result.update(result)
        _last_result["time"] = _now_text()


def _dispatch(event):
    """把一条事件按触发器列表展开：命中几条就入队几条，返回命中条数。

    自动触发走后台队列；手动触发不经过这里。
    """
    config = _config_snapshot()
    if not config["enabled"]:
        log(event.get("source") or "auto", "丢弃 %s：自动触发已关闭" % _event_text(event))
        return 0

    matched = 0
    for trigger in config["triggers"]:
        if not _trigger_matches(event, trigger):
            continue
        if _send_queue.qsize() >= QUEUE_CAP:
            log("system", "丢弃 %s：队列已满（触发器 %s）" % (_event_text(event), trigger["id"]))
            break
        _send_queue.put((event, trigger, 0))
        matched += 1
    return matched


def _sender_loop():
    while True:
        event, trigger, attempt = _send_queue.get()
        try:
            result = _send_event(event, trigger, HTTP_TIMEOUT)
            _record_result(result)
            if result["status"] in RETRYABLE_STATUS and attempt + 1 < RETRY_MAX:
                time.sleep(BACKOFF_BASE_S * (1 << attempt))
                if _send_queue.qsize() >= QUEUE_CAP:
                    log("system", "重试队列已满，丢弃 %s（触发器 %s）"
                        % (_event_text(event), trigger["id"]))
                else:
                    _send_queue.put((event, trigger, attempt + 1))
        except Exception as error:
            log("system", "投递线程异常：%r" % (error,))
        finally:
            _send_queue.task_done()


# --------------------------------------------------------------------------- #
# presence 订阅
# --------------------------------------------------------------------------- #

def _emit(scope, kind, present):
    """scope 为 global 或关注区标识，kind 为 present / absent。"""
    zone = "" if scope == SCOPE_GLOBAL else scope
    event = {
        "source": "presence",
        "event": "%s_%s" % ("global" if scope == SCOPE_GLOBAL else "zone", kind),
        "zone": zone,
        "has_present": present is not None,
        "present": bool(present),
        "scope": scope,
        "kind": kind,
    }
    if not _dispatch(event):
        return
    log("presence", "状态变化 %s" % _event_text(event))


def on_presence(snapshot):
    global _baseline_ready, _last_global_present, _known_zones

    try:
        global_present = _read_global(snapshot)
        zones = _extract_zones(snapshot)

        with _state_lock:
            previous_global = _last_global_present
            previous_zones = dict(_zone_present)
            baseline = _baseline_ready

            _last_global_present = global_present
            for zone in zones:
                _zone_present[zone["key"]] = zone["present"]
            # 页面上的区域下拉靠这份清单；快照解析不出关注区时保留上一次结果。
            if zones:
                _known_zones = [zone["key"] for zone in zones]
            _baseline_ready = True

        # 第一帧只作为基线：避免插件启动瞬间把当前状态当成状态变化推给 HA。
        if not baseline:
            log("presence", "已建立状态基线：全局%s，关注区 %d 个"
                % ("有人" if global_present else "无人", len(zones)))
            return False

        if previous_global is not None and global_present is not None and previous_global != global_present:
            _emit(SCOPE_GLOBAL, "present" if global_present else "absent", None)

        for zone in zones:
            previous = previous_zones.get(zone["key"])
            # 新出现的关注区不算状态变化。
            if previous is None or previous == zone["present"]:
                continue
            _emit(zone["key"], "present" if zone["present"] else "absent", zone["present"])
    except Exception as error:
        log("presence", "处理 presence 快照异常：%r" % (error,))
    return False


def _presence_available():
    try:
        from ainice import event
    except ImportError:
        return False
    return callable(getattr(event, "subscribe_presence", None))


def _presence_worker():
    """subscribe_presence 是阻塞订阅，SDK 不自动重连，断链后由本循环重建。"""
    from ainice import event

    backoff = 1
    while True:
        try:
            log("presence", "开始订阅 presence.snapshot")
            event.subscribe_presence(on_presence)
            log("presence", "presence 订阅已结束，1 秒后重连")
            time.sleep(1)
            backoff = 1
        except Exception as error:
            log("presence", "presence 订阅异常：%r，%d 秒后重连" % (error, backoff))
            time.sleep(backoff)
            backoff = min(backoff * 2, 30)


# --------------------------------------------------------------------------- #
# Bridge 模块
# --------------------------------------------------------------------------- #

def _config_save(params):
    candidate = _config_snapshot()

    enabled = params.get("enabled")
    if isinstance(enabled, bool):
        candidate["enabled"] = enabled
    for key in ("ha_url", "secret"):
        value = params.get(key)
        if isinstance(value, str):
            candidate[key] = value
    # 触发器列表整份替换：页面每次提交的都是完整列表。
    if isinstance(params.get("triggers"), list):
        candidate["triggers"] = params["triggers"]

    if _has_control(candidate["ha_url"]) or not _url_valid(candidate["ha_url"]):
        return {"ok": False, "error": URL_INVALID_TEXT}
    if _has_control(candidate["secret"]):
        return {"ok": False, "error": STORE_FAILED_TEXT}
    if len(candidate["triggers"]) > TRIGGER_LIMIT:
        return {"ok": False, "error": TRIGGER_LIMIT_TEXT}
    candidate["ha_url"] = candidate["ha_url"].rstrip("/")

    triggers = _normalize_triggers(candidate["triggers"], True)
    failure = _triggers_error(triggers)
    if failure:
        return {"ok": False, "error": failure}
    candidate["triggers"] = triggers

    try:
        _store_config(candidate)
    except OSError as error:
        log("system", "配置写入失败：%r" % (error,))
        return {"ok": False, "error": STORE_FAILED_TEXT}

    set_config(candidate)
    log("system", "配置已更新：enabled=%d url=%s 触发器 %d 条"
        % (1 if candidate["enabled"] else 0,
           candidate["ha_url"] or "(空)",
           len(triggers)))
    return {"ok": True, "config": _config_view()}


def _state_view():
    config = _config_snapshot()
    triggers = config["triggers"]
    ready = [item for item in triggers if item["webhook_id"] and config["ha_url"]]
    with _state_lock:
        last = dict(_last_result)
        sent_total = _sent_total
        fail_total = _fail_total
        zones = list(_known_zones)
    return {
        "ok": True,
        "enabled": config["enabled"],
        "ha_url": config["ha_url"],
        "trigger_count": len(triggers),
        "ready_count": len(ready),
        "zones": zones,
        "presence_available": _presence_available(),
        "queued": _send_queue.qsize(),
        "sent_total": sent_total,
        "fail_total": fail_total,
        "last": last,
    }


def _log_view(params):
    limit = params.get("limit")
    if isinstance(limit, bool) or not isinstance(limit, (int, float)):
        limit = LOG_DEFAULT_LIMIT
    else:
        limit = int(limit)
    if limit <= 0 or limit > LOG_LIMIT:
        limit = LOG_LIMIT

    with _log_lock:
        items = list(_log_ring)[::-1][:limit]
    return {"ok": True, "items": items}


def _trigger(params, default_source, default_event):
    """手动触发：同步单发、不重试，也不受自动触发总开关限制。

    带 trigger_id 时只发给该条触发器（页面上的单条测试），否则发给所有填了
    webhook_id 的触发器。
    """
    config = _config_snapshot()
    triggers = [item for item in config["triggers"] if item["webhook_id"]]
    wanted = params.get("trigger_id")
    if isinstance(wanted, str) and wanted:
        triggers = [item for item in triggers if item["id"] == wanted]

    if not triggers:
        log("system", "手动触发跳过：%s" % NOT_CONFIGURED_TEXT)
        return {"ok": False, "status": STATUS_CONFIG, "http_code": -1,
                "detail": NOT_CONFIGURED_TEXT, "sent": 0, "failed": 0}

    source = params.get("source")
    name = params.get("event")
    zone = params.get("zone")
    present = params.get("present")

    event = {
        "source": source if isinstance(source, str) and source else default_source,
        "event": name if isinstance(name, str) and name else default_event,
        "zone": zone if isinstance(zone, str) else "",
        "has_present": isinstance(present, (bool, int, float)),
        "present": bool(present),
    }

    sent = 0
    failed = 0
    first_failure = None
    for trigger in triggers:
        result = _send_event(event, trigger, HTTP_TIMEOUT)
        _record_result(result)
        if result["status"] == STATUS_OK:
            sent += 1
        else:
            failed += 1
            if first_failure is None:
                first_failure = result

    if first_failure is None:
        first_failure = {"status": STATUS_OK, "http_code": 0, "detail": ""}
    return {
        "ok": failed == 0,
        "status": first_failure["status"],
        "http_code": first_failure["http_code"],
        "detail": first_failure["detail"],
        "sent": sent,
        "failed": failed,
    }


def handle(request):
    try:
        if not isinstance(request, dict):
            return {"ok": False, "error": "请求格式错误"}
        method = request.get("method") or ""
        params = request.get("params")
        if not isinstance(params, dict):
            params = {}

        if method == "config.get":
            return {"ok": True, "config": _config_view()}
        if method == "config.save":
            return _config_save(params)
        if method == "state.get":
            return _state_view()
        if method == "log.get":
            return _log_view(params)
        if method == "trigger.send":
            return _trigger(params, "manual", "manual")
        if method == "trigger.test":
            return _trigger(params, "test", "test")

        return {"ok": False, "error": "未知方法"}
    except Exception as error:
        log("system", "bridge 处理异常：%r" % (error,))
        return {"ok": False, "error": "内部错误"}


# --------------------------------------------------------------------------- #
# 入口
# --------------------------------------------------------------------------- #

def main():
    global _device_name

    os.makedirs(PLUGIN_DATA, exist_ok=True)
    os.makedirs(os.path.join(PLUGIN_DATA, "tmp"), exist_ok=True)
    _prepare_log_file()
    _preload_log()

    try:
        _device_name = socket.gethostname() or "ainice"
    except OSError:
        _device_name = "ainice"

    set_config(_load_config())

    try:
        from ainice.bridge import serve
    except ImportError as error:
        log("system", "无法加载 ainice.bridge，插件退出：%r" % (error,))
        return 1

    available = _presence_available()
    log("system", "插件启动，presence 订阅%s" % ("可用" if available else "不可用"))

    threading.Thread(target=_sender_loop, name="ha-sender", daemon=True).start()
    if available:
        threading.Thread(target=_presence_worker, name="presence-subscriber", daemon=True).start()

    log("system", "注册 bridge 模块 %s" % MODULE_NAME)
    serve(MODULE_NAME, handle)
    return 0


if __name__ == "__main__":
    sys.exit(main())
