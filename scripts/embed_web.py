# PlatformIO 编译前脚本：把 web/index.html 嵌入固件，生成 src/web_page_data.h
# 设备的“配置盘”里的 Settings.html 就是它，因此网页永远和固件版本一致。
import os

Import("env")  # noqa: F821  (PlatformIO 提供)

project = env["PROJECT_DIR"]  # noqa: F821
src = os.path.join(project, "web", "index.html")
dst = os.path.join(project, "src", "web_page_data.h")

with open(src, "rb") as f:
    data = f.read()

lines = []
for i in range(0, len(data), 24):
    lines.append("  " + ", ".join(f"0x{b:02x}" for b in data[i : i + 24]) + ",")

content = (
    "// 自动生成，请勿手动修改：由 scripts/embed_web.py 从 web/index.html 生成\n"
    "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n\n"
    f"static const uint8_t WEB_PAGE[] = {{\n" + "\n".join(lines) + "\n};\n"
    f"static const size_t WEB_PAGE_LEN = {len(data)};\n"
)

old = open(dst, encoding="utf-8").read() if os.path.exists(dst) else None
if old != content:  # 内容没变就不改文件，避免每次都重新编译
    with open(dst, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"embed_web: web/index.html ({len(data)} bytes) -> src/web_page_data.h")
