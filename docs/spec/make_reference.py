"""
Word のひな形 (reference.docx) を作る。build.py が書式をここから取る。

    python docs/spec/make_reference.py

pandoc の既定のひな形を元に、A4・余白・日本語のフォント・見出し・表の罫線・ページ番号を整える。
体裁を変えたいときは、このスクリプトを直して作り直すか、reference.docx を Word で開いて
スタイルを直接直す (どちらにしても、本文の Word は build.py で作り直す)。
"""
import subprocess
import sys
from pathlib import Path

from docx import Document
from docx.enum.section import WD_ORIENT
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Mm, Pt, RGBColor

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import find_pandoc  # noqa: E402

HERE = Path(__file__).resolve().parent
REFERENCE = HERE / "reference.docx"

BODY_FONT = "游明朝"
HEAD_FONT = "游ゴシック"
LATIN_BODY = "Century"
LATIN_HEAD = "Arial"
ACCENT = RGBColor(0xA6, 0x3E, 0x00)     # 見出しの色 (PoEM のオレンジの濃い側)


def set_fonts(style, latin, east_asia, size=None, bold=None, color=None):
    style.font.name = latin
    rpr = style.element.get_or_add_rPr()
    fonts = rpr.find(qn("w:rFonts"))
    if fonts is None:
        fonts = OxmlElement("w:rFonts")
        rpr.append(fonts)
    for attr in ("w:ascii", "w:hAnsi", "w:cs"):
        fonts.set(qn(attr), latin)
    fonts.set(qn("w:eastAsia"), east_asia)
    for theme in ("w:asciiTheme", "w:hAnsiTheme", "w:eastAsiaTheme", "w:cstheme"):
        if fonts.get(qn(theme)) is not None:
            del fonts.attrib[qn(theme)]
    if size is not None:
        style.font.size = Pt(size)
    if bold is not None:
        style.font.bold = bold
    if color is not None:
        style.font.color.rgb = color


def table_borders(style):
    """表のスタイルに細い罫線を付ける (pandoc の既定は罫線なし)"""
    tblpr = style.element.find(qn("w:tblPr"))
    if tblpr is None:
        tblpr = OxmlElement("w:tblPr")
        style.element.append(tblpr)
    old = tblpr.find(qn("w:tblBorders"))
    if old is not None:
        tblpr.remove(old)
    borders = OxmlElement("w:tblBorders")
    for edge in ("top", "left", "bottom", "right", "insideH", "insideV"):
        e = OxmlElement(f"w:{edge}")
        e.set(qn("w:val"), "single")
        e.set(qn("w:sz"), "4")
        e.set(qn("w:color"), "808080")
        borders.append(e)
    tblpr.append(borders)


def page_number_footer(section):
    p = section.footer.paragraphs[0] if section.footer.paragraphs else section.footer.add_paragraph()
    p.alignment = 1     # 中央
    run = p.add_run()
    for kind, text in (("begin", None), (None, "PAGE"), ("end", None)):
        if kind:
            fld = OxmlElement("w:fldChar")
            fld.set(qn("w:fldCharType"), kind)
            run._r.append(fld)
        else:
            instr = OxmlElement("w:instrText")
            instr.set(qn("xml:space"), "preserve")
            instr.text = text
            run._r.append(instr)


def main():
    pandoc = find_pandoc()
    data = subprocess.run([pandoc, "--print-default-data-file", "reference.docx"], check=True, capture_output=True).stdout
    REFERENCE.write_bytes(data)

    doc = Document(str(REFERENCE))
    styles = doc.styles

    for name in ("Normal", "Body Text", "First Paragraph", "Compact", "Block Text"):
        if name in styles:
            set_fonts(styles[name], LATIN_BODY, BODY_FONT, size=10.5)
    for name, size in (("Title", 22), ("Subtitle", 13), ("Heading 1", 16), ("Heading 2", 13), ("Heading 3", 11.5), ("Heading 4", 10.5)):
        if name in styles:
            set_fonts(styles[name], LATIN_HEAD, HEAD_FONT, size=size, bold=True, color=ACCENT if name.startswith("Heading") else None)
    for name in ("TOC Heading",):
        if name in styles:
            set_fonts(styles[name], LATIN_HEAD, HEAD_FONT, size=16, bold=True, color=ACCENT)
            styles[name].paragraph_format.page_break_before = True     # 表紙と目次を別のページにする
    if "Source Code" in styles:
        set_fonts(styles["Source Code"], "Consolas", "ＭＳ ゴシック", size=9)
    if "Table" in styles:
        table_borders(styles["Table"])

    for section in doc.sections:
        section.orientation = WD_ORIENT.PORTRAIT
        section.page_width, section.page_height = Mm(210), Mm(297)
        section.top_margin = section.bottom_margin = Mm(25)
        section.left_margin = section.right_margin = Mm(22)
        page_number_footer(section)

    doc.save(str(REFERENCE))
    print(f"作成しました: {REFERENCE}")


if __name__ == "__main__":
    main()
