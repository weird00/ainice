#!/bin/sh
# 打包 ha-webhook 插件（默认走 Python 后端），或在交叉编译 C 备用实现后再打包。
#
# 用法：
#   ./build.sh        打包 ha-webhook.app（Python 后端）→ ha-webhook.plugin
#   ./build.sh c      交叉编译 src/*.c 覆盖 ha-webhook.app 后再打包
#
# C 模式前置：按设备开发者说明导出 AINICE_SDK / AINICE_SYSROOT / TC，例如
#   export AINICE_SDK="$HOME/ainice-subsys-sdk"
#   export AINICE_SYSROOT="$AINICE_SDK/sysroot"
#   export TC="$AINICE_SDK/toolchains/arm-gnu-toolchain-13.2.Rel1-x86_64-aarch64-none-linux-gnu"
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ID="ha-webhook"
APP="$DIR/$ID.app"

case "${1:-plugin}" in
    plugin | pack)
        ;;
    c | C | -c | --c)
        if [ -z "${AINICE_SYSROOT:-}" ] || [ -z "${TC:-}" ]; then
            echo "C 模式缺少 AINICE_SYSROOT 或 TC 环境变量，请先按开发者说明准备 sysroot 与工具链。" >&2
            exit 1
        fi

        CC="${CC:-$TC/bin/aarch64-none-linux-gnu-gcc}"
        STRIP="${STRIP:-$TC/bin/aarch64-none-linux-gnu-strip}"

        # -lcurl 用于向 HA 发起 HTTP POST（PicoClaw 后端同样依赖 libcurl，板端已具备）。
        "$CC" -std=gnu11 -O3 -Wall -Wextra \
            -mcpu=cortex-a53 -mtune=cortex-a53 \
            -ffunction-sections -fdata-sections \
            --sysroot="$TC/aarch64-none-linux-gnu/libc" \
            -I"$AINICE_SYSROOT/usr/include" \
            "$DIR"/src/*.c \
            -L"$AINICE_SYSROOT/usr/lib" \
            -Wl,-rpath-link,"$AINICE_SYSROOT/usr/lib" \
            -Wl,--gc-sections -Wl,--as-needed \
            -lainice -lcjson -lcurl -pthread \
            -o "$APP"

        "$STRIP" --strip-all "$APP"
        ;;
    *)
        echo "用法：$0 [plugin|c]" >&2
        exit 1
        ;;
esac

[ -f "$DIR/plugin.json" ] || { echo "缺少 plugin.json，无法打包。" >&2; exit 1; }
[ -d "$DIR/www" ] || { echo "缺少 www/，无法打包。" >&2; exit 1; }
[ -f "$APP" ] || { echo "缺少 $ID.app，无法打包。" >&2; exit 1; }
[ -x "$APP" ] || chmod +x "$APP"

cd "$DIR"
rm -f "$ID.plugin"
# 显式清单：只打 plugin.json、插件进程入口与页面，src/ 等开发文件不入包。
# --format=gnutar 必须保留：
# 1) macOS 的 tar 默认写 PAX 扩展头，包内会多出 PaxHeader/* 条目与
#    SCHILY.xattr.* 记录，上传时被判为「包含不支持的文件」；
# 2) 平台自带插件包（presence-notify.plugin）用的是 GNU 头（magic `ustar  `），
#    POSIX ustar 头（magic `ustar\0`）与它不一致，这里统一按 GNU 头输出。
# COPYFILE_DISABLE 避免扩展属性被写成 ._* 附带文件。
COPYFILE_DISABLE=1 tar --format=gnutar -czf "$ID.plugin" plugin.json "$ID.app" www/

# 按裸 tar 头逐条校验，只允许普通文件(0)与目录(5)。
python3 - "$ID.plugin" <<'PY'
import gzip
import sys

raw = gzip.decompress(open(sys.argv[1], "rb").read())
offset = 0
unsupported = []
while offset + 512 <= len(raw):
    block = raw[offset:offset + 512]
    if block.strip(b"\0") == b"":
        break
    name = block[0:100].rstrip(b"\0").decode("utf-8", "replace")
    kind = chr(block[156])
    if kind not in ("0", "5"):
        unsupported.append("%s(type=%s)" % (name, kind))
    size = int(block[124:136].rstrip(b"\0 ").decode() or "0", 8)
    offset += 512 + ((size + 511) // 512) * 512

if unsupported:
    sys.exit("包内含平台不支持的文件：%s" % ", ".join(unsupported))
print("校验通过：包内只有普通文件与目录")
PY

echo "已生成 $DIR/$ID.plugin"
tar -tzf "$ID.plugin"
