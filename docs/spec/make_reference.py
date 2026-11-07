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
from docx.shared import Mm, Pt

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import find_pandoc  # noqa: E402

HERE = Path(__file__).resolve().parent
REFERENCE = HERE / "reference.docx"

# フォントは「強調用」(見出し・題名・目次の章の行) と「一般用」(本文) の 2 種類に分ける。
# 太字は使わない (強調はフォントの違いで表す)。色は使わず白黒にする (図を除く)。
EMPH_LATIN = "Arial"
EMPH_JA = "BIZ UDPゴシック"
BODY_LATIN = "Times New Roman"
BODY_JA = "BIZ UDP明朝 Medium"


def set_fonts(style, latin, east_asia, size=None, bold=None):
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

    for name in ("Normal", "Body Text", "First Paragraph", "Compact", "Block Text", "Date", "Author"):
        if name in styles:
            set_fonts(styles[name], BODY_LATIN, BODY_JA, size=10.5)
    # 見出し 1 = 章、見出し 2 = 節、見出し 3 = 項
    for name, size in (("Title", 22), ("Subtitle", 13), ("Heading 1", 20), ("Heading 2", 14),
                       ("Heading 3", 11.5), ("Heading 4", 10.5), ("TOC Heading", 16)):
        if name in styles:
            set_fonts(styles[name], EMPH_LATIN, EMPH_JA, size=size, bold=False)
            styles[name].font.italic = False    # 斜体も使わない (強調は書体の違いだけで表す)
    styles["TOC Heading"].paragraph_format.page_break_before = True     # 表紙と目次を別のページにする
    if "Source Code" in styles:
        set_fonts(styles["Source Code"], "Consolas", "BIZ UDゴシック", size=9)

    styles["Heading 1"].paragraph_format.space_before = Pt(0)     # 章は新しいページの先頭から始まる
    styles["Heading 1"].paragraph_format.space_after = Pt(18)

    # 目次: 1 段目 = 章 (強調用フォント)、2 段目 = 節 (一般用フォントで字下げ)。行間を詰めて、全体を 1〜2 ページで見渡せるようにする
    for name, indent, emph, before in (("toc 1", 0, True, 6), ("toc 2", 4, False, 0)):
        st = styles[name] if name in styles else styles.add_style(name, WD_STYLE_TYPE.PARAGRAPH)
        st.base_style = styles["Normal"]
        set_fonts(st, EMPH_LATIN if emph else BODY_LATIN, EMPH_JA if emph else BODY_JA, size=10.5, bold=False)
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
    finish_styles()
    print(f"作成しました: {REFERENCE}")


def finish_styles():
    """
    保存した reference.docx の styles.xml を直接書き換える。
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
                # 太字を使わない、色を使わない (白黒) という方針を、pandoc の既定のスタイル
                # (見出しの色、リンクの青、表の見出し行の太字など) も含めて全体に当てる
                xml = re.sub(r'<w:bCs?(?: w:val="[^"]*")? ?/>', '', xml)
                xml = re.sub(r'<w:color [^>]*/>', '<w:color w:val="000000"/>', xml)
                xml = re.sub(r'<w:u w:val="[^"]*" ?/>', '', xml)     # リンクの下線も外す
                data = xml.encode("utf-8")
            zout.writestr(item, data)
    tmp.replace(REFERENCE)


if __name__ == "__main__":
    main()
