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
        "--toc",
        "--syntax-highlighting=none",               # コードの色分けをしない (資料は白黒、太字を使わない)
        "--number-sections",                         # {.unnumbered} の見出し (部・付録・前付け) は番号なし
        "--resource-path", f"{HERE}{';' if sys.platform == 'win32' else ':'}{HERE / 'figures'}",
    ]
    subprocess.run(cmd, check=True)
    fix_toc(out)
    print(f"作成しました: {out}")


def fix_toc(docx_path: Path):
    """
    目次を「部」(Part Title スタイル) と「章」(見出し 1) の 2 段にする。
    pandoc の目次は見出しの段数で拾うので、部を 1 段目、章を 2 段目として拾うように目次の設定を書き換える。
    節 (見出し 2 以下) は目次に入れない (全体を 1〜2 ページで見渡せるようにする)。
    """
    import re
    import zipfile
    tmp = docx_path.with_suffix(".tmp")
    with zipfile.ZipFile(docx_path) as zin, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "word/document.xml":
                xml = data.decode("utf-8")
                # 見出しのスタイル名は Word の表示言語で変わる (日本語版は「見出し 1」) ので両方を書く
                new_field = 'TOC \\h \\z \\t &quot;Part Title,1,Heading 1,2,見出し 1,2&quot;'
                xml, n = re.subn(r'TOC \\o (?:&quot;|")[^<]*', lambda m: new_field, xml, count=1)
                if n == 0:
                    print("注意: 目次の設定が見つからなかった (pandoc の出力の形が変わった可能性)")
                data = xml.encode("utf-8")
            zout.writestr(item, data)
    tmp.replace(docx_path)


if __name__ == "__main__":
    main()
