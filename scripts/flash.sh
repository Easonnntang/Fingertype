#!/bin/zsh
# 开发用：编译固件并刷入 XIAO ESP32S3，然后读一次设备状态。
# 用法：在项目目录运行  zsh scripts/flash.sh
set -e
cd "$(dirname "$0")/.."

echo "== 编译 =="
pio run | tail -3

port=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
if [[ -z "$port" ]]; then
  echo "找不到设备：请用数据线把 XIAO 插到 Mac 上"; exit 1
fi
if lsof "$port" >/dev/null 2>&1; then
  echo "串口被占用（多半是 Chrome 里的配置网页通过 USB 连着）：请先在网页上点「断开」"; exit 1
fi

echo "== 刷入（第一次让板子重启进刷机模式，第二次真正写入）=="
pio run -t upload --upload-port "$port" >/dev/null 2>&1 || true
sleep 3
port=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
pio run -t upload --upload-port "$port" | grep -E "SUCCESS|FAILED|rror" | tail -2

echo "== 等设备重启 =="
sleep 8
py="$(brew --prefix platformio)/libexec/bin/python"
"$py" scripts/devcmd.py '{"cmd":"fpRaw","bytes":[31,0]}' '{"cmd":"fpRaw","bytes":[29]}' '{"cmd":"info"}'
