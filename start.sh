#!/bin/sh
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! command -v python3 >/dev/null 2>&1; then
    echo '需要 Python 3.8+，请用当前 Linux 发行版的包管理器安装 python3。' >&2
    exit 1
fi
exec python3 "$SCRIPT_DIR/wsl_tunnel.py" "$@"
