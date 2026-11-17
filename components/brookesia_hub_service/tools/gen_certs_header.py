#!/usr/bin/env python3
"""把 MQTT 双向认证的三件套编译期嵌进固件。

    python -X utf8 components/brookesia_hub_service/tools/gen_certs_header.py <证书目录> <输出头文件>

构建时由本组件的 CMakeLists.txt 自动调用，一般不需要手动跑。

证书目录里只需要三个文件（其余 `*.key` 一律不碰）：

    ca.crt   校验 broker 的 CA
    p4.crt   P4 的客户端证书
    p4.key   P4 的客户端私钥

⚠️ 这三个文件是**密钥材料**，已被 .gitignore 挡住（`secrets/`、`*.key`、`*.crt`）。
   本脚本只把它们转成 C 字符串字面量，**既不打印内容、也不写到别处**。

缺文件时**不报错**，生成的头部里 `HUB_CERTS_PRESENT` 为 0 —— 这样干净的 clone
也能编过，运行时由 UI 提示"缺证书"，而不是让构建直接失败。

内容没变就不重写文件（保持 mtime 稳定，避免每次都重编 .cpp）。
"""
import os
import sys

FILES = [
    ("HUB_CA_CERT", "ca.crt"),
    ("HUB_CLIENT_CERT", "p4.crt"),
    ("HUB_CLIENT_KEY", "p4.key"),
]


def emit_literal(name, data: str) -> str:
    lines = ['static const char %s[] =' % name]
    for line in data.splitlines(keepends=True):
        escaped = line.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
        lines.append('    "%s"' % escaped)
    lines.append("    ;")
    return "\n".join(lines)


def main() -> int:
    if len(sys.argv) != 3:
        print("用法: gen_certs_header.py <证书目录> <输出头文件>", file=sys.stderr)
        return 2

    cert_dir, out_path = sys.argv[1], sys.argv[2]
    parts = [
        "/* 自动生成 —— 别手改（components/brookesia_hub_service/tools/gen_certs_header.py）。",
        " * 内容来自 secrets/ 目录下的 ca.crt / p4.crt / p4.key，",
        " * 该目录被 .gitignore 挡住，**私钥不入库**。 */",
        "#pragma once",
        "",
    ]

    present = True
    bodies = []
    for macro, filename in FILES:
        path = os.path.join(cert_dir, filename)
        try:
            with open(path, "r", encoding="utf-8") as handle:
                data = handle.read()
        except OSError:
            present = False
            bodies.append("static const char %s[] = \"\";" % macro)
            continue
        bodies.append(emit_literal(macro, data))

    parts.append("#define HUB_CERTS_PRESENT %d" % (1 if present else 0))
    parts.append("")
    parts.extend(bodies)
    parts.append("")
    content = "\n".join(parts)

    if not present:
        print("[gen_certs] 证书不全，生成空证书头（构建继续，运行时会提示缺证书）")
    else:
        print("[gen_certs] 已嵌入 ca.crt / p4.crt / p4.key")

    try:
        with open(out_path, "r", encoding="utf-8") as handle:
            if handle.read() == content:
                return 0
    except OSError:
        pass

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
