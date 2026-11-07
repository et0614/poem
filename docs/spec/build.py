"""
仕様書の md (src/*.md) をまとめて Word (.docx) にする。

    python docs/spec/build.py

- src/ のファイルをファイル名の順につなげる (00_front, 10_overview, ... 90_appendix)。
- 書式は reference.docx (ひな形) から取る。ひな形が無ければ make_reference.py で作る。
- 出力は build/PoEM_仕様書_<版>.docx。Word は md から毎回作り直すので、Word を手で直さないこと
  (内容は md で、体裁は reference.docx で直す)。
"""
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "src"
OUT = HERE / "build"
REFERENCE = HERE / "reference.docx"
METADATA = HERE / "metadata.yaml"


def find_pandoc() -> str:
    exe = shutil.which("pandoc")
    if exe:
        return exe
    for p in (Path.home() / "AppData/Local/Pandoc/pandoc.exe", Path("C:/Program Files/Pandoc/pandoc.exe")):
        if p.exists():
            return str(p)
    sys.exit("pandoc が見つかりません。https://pandoc.org/installing.html から入れてください (winget install JohnMacFarlane.Pandoc)。")


def read_metadata() -> dict:
    """metadata.yaml の「キー: 値」を読む (入れ子の無い単純な形だけを扱う)"""
    meta = {}
    for line in METADATA.read_text(encoding="utf-8").splitlines():
        if ":" in line and not line.lstrip().startswith("#"):
            k, v = line.split(":", 1)
            meta[k.strip()] = v.strip().strip('"')
    return meta


def version() -> str:
    return read_metadata().get("version", "draft")


def main():
    pandoc = find_pandoc()
    if not REFERENCE.exists():
        sys.exit("reference.docx がありません。先に python docs/spec/make_reference.py を実行してください。")

    OUT.mkdir(exist_ok=True)
    out = OUT / f"PoEM_仕様書_{version()}.docx"

    # 表の列幅を中身の文字量に合わせてから pandoc に渡す (原稿の md は書き換えない)
    work = OUT / "_src"
    work.mkdir(exist_ok=True)
    sources = []
    for src in sorted(SRC.glob("*.md")):
        dst = work / src.name
        dst.write_text(set_table_widths(src.read_text(encoding="utf-8")), encoding="utf-8")
        sources.append(dst)

    meta = read_metadata()
    cmd = [
        pandoc, *map(str, sources),
        "--metadata-file", str(METADATA),
        # 表紙の日付の欄に版と日付を並べる (pandoc の表紙には版の欄が無いため)
        "-M", f"date=第 {meta.get('version', '')} 版{' (' + meta['status'] + ')' if meta.get('status') else ''}　{meta.get('date', '')}",
        "-f", "markdown+east_asian_line_breaks",    # 日本語の改行で余計な空白を入れない
        "-o", str(out),
        "--reference-doc", str(REFERENCE),
        "--toc", "--toc-depth=2",                  # 目次は章と節まで (項は入れない)
        "--syntax-highlighting=none",               # コードの色分けをしない (資料は白黒、太字を使わない)
        "--number-sections",                         # 章・節・項に番号 (1、1.1、1.1.1)。{.unnumbered} の見出し (前付け・付録) は番号なし
        "--resource-path", f"{HERE}{';' if sys.platform == 'win32' else ':'}{HERE / 'figures'}",
    ]
    subprocess.run(cmd, check=True)
    print(f"作成しました: {out}")


def _display_width(text: str) -> int:
    """表示上の幅 (全角 2、半角 1)"""
    import unicodedata
    return sum(2 if unicodedata.east_asian_width(c) in ("W", "F") else 1 for c in text)


def set_table_widths(md: str) -> str:
    """
    パイプ表の区切り行 (|---|---|) の「-」の数を、列ごとの文字量に比例させる。
    pandoc は、行が長い表では区切り行の「-」の数の比で列幅を決める。原稿で
    すべて |---| と書いても、中身の多い列が広くなるようにする。
    """
    lines = md.split("\n")
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        is_table = (line.lstrip().startswith("|") and i + 1 < len(lines)
                    and lines[i + 1].replace("|", "").replace("-", "").replace(":", "").strip() == ""
                    and "-" in lines[i + 1])
        if not is_table:
            out.append(line)
            i += 1
            continue
        rows = [line]
        j = i + 2
        while j < len(lines) and lines[j].lstrip().startswith("|"):
            rows.append(lines[j])
            j += 1
        cells = [[c.strip() for c in r.strip().strip("|").split("|")] for r in rows]
        ncol = len(cells[0])
        # 列幅 = max(途中で切れない語の最長 + 2, 文字量の半分)。番地のような短い値は折り返さず、
        # 文章の列は広くなりすぎないようにする
        import re
        widths = [4] * ncol
        for n, r in enumerate(cells):
            for k, c in enumerate(r[:ncol]):
                w = _display_width(c)
                token = max((_display_width(t) for t in re.findall(r"[\x21-\x7e〜×°²]+", c)), default=0)
                widths[k] = max(widths[k], token + 2, min(w, 60) // 2)
                if n == 0 or w <= 12:
                    widths[k] = max(widths[k], w + 2)     # 見出しの行と短い値は折り返さない
        aligns = [c.strip() for c in lines[i + 1].strip().strip("|").split("|")]
        sep = []
        for k in range(ncol):
            a = aligns[k] if k < len(aligns) else "---"
            left, right = a.startswith(":"), a.endswith(":")
            n = widths[k] - left - right
            sep.append((":" if left else "") + "-" * max(n, 3) + (":" if right else ""))
        out.append(line)
        out.append("|" + "|".join(sep) + "|")
        out.extend(rows[1:])
        i = j
    return "\n".join(out)


if __name__ == "__main__":
    main()
