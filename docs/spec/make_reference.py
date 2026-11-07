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
from docx.enum.style import WD_STYLE_TYPE
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Mm, Pt, RGBColor

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import find_pandoc  # noqa: E402

HERE = Path(__file__).resolve().parent
REFERENCE = HERE / "reference.docx"

# 本文も見出しも BIZ UDP ゴシック (Windows 10 以降に標準で入っている UD フォント) で統一する。
# 英数字も同じフォントにして、日本語と混ざったときの字形の不揃いをなくす。
BODY_FONT = "BIZ UDPゴシック"
HEAD_FONT = "BIZ UDPゴシック"
LATIN_BODY = BODY_FONT
LATIN_HEAD = HEAD_FONT
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
        set_fonts(styles["Source Code"], "Consolas", "BIZ UDゴシック", size=9)

    # 部の題名 (md の ::: {custom-style="Part Title"})。章の見出しより大きく、目次では 1 段目になる
    part = styles["Part Title"] if "Part Title" in styles else styles.add_style("Part Title", WD_STYLE_TYPE.PARAGRAPH)
    part.base_style = styles["Normal"]
    set_fonts(part, LATIN_HEAD, HEAD_FONT, size=20, bold=True, color=ACCENT)
    part.paragraph_format.space_before = Pt(0)
    part.paragraph_format.space_after = Pt(18)
    part.paragraph_format.keep_with_next = True

    # 目次: 1 段目 = 部 (太字)、2 段目 = 章 (字下げ)。行間を詰めて、全体を 1〜2 ページで見渡せるようにする
    for name, indent, bold, before in (("toc 1", 0, True, 6), ("toc 2", 4, False, 0)):
        st = styles[name] if name in styles else styles.add_style(name, WD_STYLE_TYPE.PARAGRAPH)
        st.base_style = styles["Normal"]
        set_fonts(st, LATIN_BODY, BODY_FONT, size=10.5, bold=bold)
        pf = st.paragraph_format
        pf.left_indent = Mm(indent * 2)
        pf.space_before = Pt(before)
        pf.space_after = Pt(0)
        pf.line_spacing = 1.0
    if "Table" in styles:
        table_borders(styles["Table"])

    for section in doc.sections:
        section.orientation = WD_ORIENT.PORTRAIT
        section.page_width, section.page_height = Mm(210), Mm(297)
        section.top_margin = section.bottom_margin = Mm(25)
        section.left_margin = section.right_margin = Mm(22)
        page_number_footer(section)

    doc.save(str(REFERENCE))
    make_toc_styles_builtin()
    print(f"作成しました: {REFERENCE}")


def make_toc_styles_builtin():
    """
    python-docx で足したスタイルは「ユーザー定義」(customStyle) になり、Word が目次に使う組み込みの
    「目次 1」「目次 2」(styleId TOC1, TOC2) と別物として扱われる。styleId を組み込みのものに合わせ、
    ユーザー定義の印を外して、Word が目次を更新したときにこの書式を使うようにする。
    """
    import re
    import zipfile
    tmp = REFERENCE.with_suffix(".tmp")
    with zipfile.ZipFile(REFERENCE) as zin, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "word/styles.xml":
                xml = data.decode("utf-8")
                for n in ("1", "2"):
                    xml = re.sub(rf'<w:style w:type="paragraph" w:customStyle="1" w:styleId="toc{n}">',
                                 f'<w:style w:type="paragraph" w:styleId="TOC{n}">', xml)
                data = xml.encode("utf-8")
            zout.writestr(item, data)
    tmp.replace(REFERENCE)


if __name__ == "__main__":
    main()
