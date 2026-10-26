#!/bin/zsh
# 调试用：检查指纹模块收到一条命令后，会不会多发回复（"extra" 里出现的就是多出来的回复）。
# 用法：zsh scripts/probe.sh   （测试期间不要碰传感器）
cd "$(dirname "$0")/.."
py="$(brew --prefix platformio)/libexec/bin/python"
echo "== 采图（没手指）、读数量、光圈常亮、光圈关闭：各自听 1.5~3 秒 =="
"$py" scripts/devcmd.py \
  '{"cmd":"info"}' \
  '{"cmd":"fpRaw","bytes":[1],"listenMs":1500,"_wait":5}' \
  '{"cmd":"fpRaw","bytes":[29],"listenMs":1500,"_wait":5}' \
  '{"cmd":"fpRaw","bytes":[60,3,1,1,0],"listenMs":3000,"_wait":6}' \
  '{"cmd":"fpRaw","bytes":[60,4,0,0,0],"listenMs":1500,"_wait":5}'
