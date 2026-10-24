#!/usr/bin/env python3
"""生成 music-radio 的 LVGL 中文字库子集（16px 正文 / 24px 标题 / 32px 台名）。

字符清单**从不手工维护**，来源是 `main/` 下所有 C/C++ 源文件的字符串字面量
（先剥掉注释，所以中文注释不占 Flash），加上可打印 ASCII 和界面用的排版符号。
产出的字形数少于请求数就当作构建期失败，而不是设备上一个看不清的空框。

改自 english-podcast 的 tools/epd/gen_font.py（真机踩坑经验全部保留）。
依赖：`lv_font_conv`（npm 1.5.3）与 node。母字体：Noto Sans CJK SC Regular
（SIL Open Font License 1.1）。

用法（在仓库根目录或本目录都行，路径全部由 __file__ 推导）：
    python projects/music-radio/tools/gen_font.py           # 生成三档字库
    python projects/music-radio/tools/gen_font.py --check   # 只算清单与覆盖率
    python projects/music-radio/tools/gen_font.py --sizes 16 24

环境变量覆盖：
    MR_SOURCE_FONT    母字体 OTF/TTF 路径
    LV_FONT_CONV      lv_font_conv 入口脚本路径
    NODE_EXE          运行转换器的 node 可执行文件
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]          # projects/music-radio
MAIN = ROOT / "main"
FONT_DIR = ROOT / "assets" / "fonts"
CHARSET = FONT_DIR / "charset.txt"

SIZES = (16, 24, 32)
# 32px 只画台名/时钟/音量数字（界面静态约定）；动态文本一律 16px。
TITLE_ONLY_SIZES = frozenset({24, 32})
BPP = 4
SYMBOL_FONT_PREFIX = "mr_font_"

SOURCE_FONT_NAME = "NotoSansCJKsc-Regular.otf"
DEFAULT_SOURCE_FONT = Path("D:/esp/fontsrc") / SOURCE_FONT_NAME

LV_FONT_CONV_CANDIDATES = (
    Path("D:/esp/fonttools/node_modules/lv_font_conv/lv_font_conv.js"),
    ROOT.parents[0] / "fitness-trainer" / "tools" / "node_modules" / "lv_font_conv" / "lv_font_conv.js",
)

# 界面会自己画到的排版符号：台标星标、圆点、负号、箭头等。
# ★☆（收藏）、‹›（翻页/上一台）、−（音量）、·（分隔）、…（省略）。
EXTRA_SYMBOLS = "·…—‹›−★☆×✓○●"

PRINTABLE_ASCII = "".join(chr(c) for c in range(0x20, 0x7F))

STRING_LITERAL_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"|\'((?:[^\'\\\n]|\\.)*)\'')


def strip_comments(text: str) -> str:
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        if ch in '"\'':  # 字面量原样保留，交给后面的正则
            quote = ch
            out.append(ch)
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    out.append(text[i])
                    i += 1
                out.append(text[i])
                i += 1
            out.append(quote if i < n else "")
            i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


_ESCAPE_RE = re.compile(r'\\([nrt"\'\\ab f])')
_ESCAPES = {"n": "\n", "r": "\r", "t": "\t", '"': '"', "'": "'", "\\": "\\",
            "a": "\a", "b": "\b", " ": " ", "f": "\f"}


def unescape(raw: str) -> str:
    """只解 C 转义序列，不动非 ASCII 字节（unicode_escape 会把 UTF-8 中文按
    Latin-1 解一遍 —— english-podcast 2026-10-16 真机一屏乱码的根因）。"""
    return _ESCAPE_RE.sub(lambda m: _ESCAPES.get(m.group(1), m.group(1)), raw)


def literals_of(text: str) -> list[str]:
    found = []
    for m in STRING_LITERAL_RE.finditer(text):
        raw = m.group(1) if m.group(1) is not None else m.group(2)
        if raw is None:
            continue
        found.append(unescape(raw))
    return found


def collect_charset() -> tuple[set[str], dict[str, int]]:
    chars: set[str] = set(PRINTABLE_ASCII) | set(EXTRA_SYMBOLS)
    hits: dict[str, int] = {}
    for path in sorted(MAIN.glob("*.[ch]")) + sorted(MAIN.glob("*.cc")):
        text = strip_comments(path.read_text(encoding="utf-8"))
        for lit in literals_of(text):
            for c in lit:
                if ord(c) < 0x20:
                    continue
                chars.add(c)
                hits.setdefault(path.name, 0)
                hits[path.name] += 1
    return chars, hits


def font_glyph_count(c_file: Path) -> int:
    text = c_file.read_text(encoding="utf-8")
    m = re.search(r"\.glyph_count\s*=\s*(\d+)", text)
    if m:
        return int(m.group(1))
    m = re.search(r"static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc\[\]\s*=\s*\{(.*?)\n\};",
                  text, re.S)
    return len(re.findall(r"\{", m.group(1))) if m else 0


def find_source_font(explicit: str | None) -> Path:
    path = Path(explicit or os.environ.get("MR_SOURCE_FONT") or DEFAULT_SOURCE_FONT)
    if not path.is_file():
        raise SystemExit(
            f"找不到母字体：{path}\n用 --font <path> 或设 MR_SOURCE_FONT。\n"
            f"官方下载地址：notofonts/noto-cjk 的 Sans/OTF/SimplifiedChinese/{SOURCE_FONT_NAME}")
    return path


def find_converter() -> list[str]:
    node = os.environ.get("NODE_EXE", "node")
    env = os.environ.get("LV_FONT_CONV")
    candidates = [Path(env)] if env else list(LV_FONT_CONV_CANDIDATES)
    for candidate in candidates:
        if candidate.is_file():
            return [node, str(candidate)]
    raise SystemExit(
        "找不到 lv_font_conv。安装方式：\n"
        "  npm install lv_font_conv@1.5.3\n"
        "然后设 LV_FONT_CONV=<…>/lv_font_conv.js")


def convert(command: list[str], source_font: Path, symbols: str, size: int,
            output: Path) -> None:
    args = command + [
        "--font", str(source_font),
        "--symbols", symbols,
        "--size", str(size),
        "--bpp", str(BPP),
        "--format", "lvgl",
        "--no-compress",
        "--lv-include", "lvgl.h",
        "--lv-font-name", f"{SYMBOL_FONT_PREFIX}{size}",
        "-o", str(output),
    ]
    result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8")
    if result.returncode != 0:
        print(result.stdout[-4000:], file=sys.stderr)
        print(result.stderr[-4000:], file=sys.stderr)
        raise SystemExit(f"lv_font_conv 在字号 {size} 上失败")
    if result.stdout.strip():
        print(result.stdout.strip())


def write_charset(chars: set[str]) -> None:
    lines = [
        "# music-radio 的字库字符清单。",
        "# 由 tools/gen_font.py 从 main/ 的字符串字面量推导，请勿手改。",
        "# 改界面文案后重新运行 gen_font.py，否则新字在设备上会渲染成乱码/空框。",
        "".join(sorted(chars, key=ord)),
    ]
    CHARSET.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", help="母字体 OTF/TTF 路径")
    parser.add_argument("--sizes", nargs="*", type=int, default=list(SIZES))
    parser.add_argument("--check", action="store_true", help="只核对清单，不转换")
    args = parser.parse_args()

    # 两套清单：full 含 WiFi SSID 高频汉字表（radio_ui.cc 的 kWifiSsidCharset
    # 字面量会自动被扫进来）；ui 只含界面文案。24/32px 走 ui（省 Flash），
    # 16px 走 full（SSID、动态文本）。
    full, hits = collect_charset()
    ui, _ = collect_charset()
    cjk = {c for c in full if ord(c) > 0x7F}
    print(f"字符清单：{len(full)} 个（非 ASCII {len(cjk)}）")
    for name, count in hits.items():
        print(f"  {name}: 字面量 {count} 处")

    write_charset(full)
    if args.check:
        return 0

    source_font = find_source_font(args.font)
    command = find_converter()
    FONT_DIR.mkdir(parents=True, exist_ok=True)

    for size in args.sizes:
        chars = ui if size in TITLE_ONLY_SIZES else full
        symbols = "".join(sorted(chars, key=ord))
        output = FONT_DIR / f"{SYMBOL_FONT_PREFIX}{size}.c"
        convert(command, source_font, symbols, size, output)
        produced = font_glyph_count(output)
        kb = output.stat().st_size / 1024
        print(f"  {output.name}: 请求 {len(chars)} 字形，产出 {produced} 字形，{kb:.0f} KB 源文件")
        if produced < len(chars):
            raise SystemExit(
                f"字号 {size} 少产出了 {len(chars) - produced} 个字形：母字体不覆盖清单里的某些字符。"
                "请把那个字符从界面文案里去掉，或换母字体。")

    print("字库生成完成。main/CMakeLists.txt 已包含全部三档字号。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
