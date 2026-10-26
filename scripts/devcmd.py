"""开发调试用：通过 USB 串口给设备发 JSON 命令，打印回复和设备事件。

用法（用 PlatformIO 自带的 Python，它装好了 pyserial）：
    $(brew --prefix platformio)/libexec/bin/python scripts/devcmd.py [端口] '{"cmd":"info"}' ['{"cmd":"identify","_wait":20}' ...]
端口省略时自动找 /dev/cu.usbmodem*。命令里的 "_wait" 是等待回复的秒数（默认 5）。
"""
import glob
import json
import sys
import time

import serial

args = sys.argv[1:]
if args and args[0].startswith("/dev/"):
    port, cmds = args[0], args[1:]
else:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("找不到设备串口（/dev/cu.usbmodem*）")
    port, cmds = ports[0], args

s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr, s.rts = True, False  # DTR=1、RTS=0：避免 ESP32 被串口信号复位进刷机模式
s.open()
time.sleep(0.3)
for i, c in enumerate(cmds, 1):
    req = json.loads(c)
    req.setdefault("id", i)
    wait = float(req.pop("_wait", 5))
    s.write((json.dumps(req) + "\n").encode())
    end = time.time() + wait
    while time.time() < end:
        line = s.readline().decode(errors="replace").strip()
        if not line:
            continue
        print(line, flush=True)
        try:
            if json.loads(line).get("re") == req["id"]:
                break
        except ValueError:
            pass
s.close()
