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

    sources = sorted(SRC.glob("*.md"))
    OUT.mkdir(exist_ok=True)
    out = OUT / f"PoEM_仕様書_{version()}.docx"

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


if __name__ == "__main__":
    main()
