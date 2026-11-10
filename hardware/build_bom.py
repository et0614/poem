"""全基板の BOM を 1 つの Excel (poem_bom.xlsx) にまとめる。

元データは各基板の fab/poem_<基板>_BOM.csv (JLCPCB 形式) または fab/poem_<基板>_BOM.xlsx。
poem_bom.xlsx は生成物なので手で編集しない。BOM を直したら、このスクリプトを実行し直す。

    python hardware/build_bom.py
"""
import csv
import re
from pathlib import Path

from openpyxl import Workbook, load_workbook
from openpyxl.styles import Font
from openpyxl.utils import get_column_letter

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "poem_bom.xlsx"

# 列名の揺れを吸収する (元の列名 → 正規化した列名)
ALIASES = {
    "designator": "designator",
    "qty": "qty", "quantity": "qty",
    "comment": "part", "manufacturer part number": "part", "value": "part",
    "manufacturer": "maker",
    "footprint": "footprint", "package": "footprint",
    "lcsc part #": "lcsc", "lcsc": "lcsc", "jlcpcb part #": "lcsc",
    "link": "link",
}
COLUMNS = [("designator", "Designator"), ("qty", "数量"), ("part", "部品"), ("maker", "メーカー"),
           ("footprint", "フットプリント"), ("lcsc", "LCSC"), ("link", "リンク")]


def count_designators(s):
    n = 0
    for tok in re.split(r"[,\s]+", s or ""):
        m = re.fullmatch(r"([A-Za-z$]*)(\d+)-([A-Za-z$]*)(\d+)", tok)
        if m:
            n += int(m.group(4)) - int(m.group(2)) + 1
        elif tok:
            n += 1
    return n


def normalize(header, rows):
    keys = [ALIASES.get(str(h or "").strip().lower()) for h in header]
    out = []
    for r in rows:
        d = {k: (str(v).strip() if v is not None else "") for k, v in zip(keys, r) if k}
        if not d.get("part") and not d.get("lcsc"):
            continue
        q = d.get("qty", "")
        d["qty"] = int(float(q)) if re.fullmatch(r"\d+(\.0)?", q) else count_designators(d.get("designator"))
        out.append(d)
    return out


def read_bom(path):
    if path.suffix == ".csv":
        with open(path, encoding="utf-8-sig", newline="") as f:
            rows = list(csv.reader(f))
    else:
        ws = load_workbook(path, data_only=True).worksheets[0]
        rows = [list(r) for r in ws.iter_rows(values_only=True)]
    rows = [r for r in rows if any(c not in (None, "") for c in r)]
    return normalize(rows[0], rows[1:]) if rows else []


def part_key(d):
    if d.get("lcsc"):
        return d["lcsc"]
    return re.sub(r"\s*\(.*\)\s*$", "", d.get("part", "")).strip().upper()


def autosize(ws):
    for col in ws.columns:
        width = max(len(str(c.value or "")) for c in col)
        ws.column_dimensions[get_column_letter(col[0].column)].width = min(max(width + 2, 6), 60)


def main():
    boards = {}
    for d in sorted(p for p in ROOT.iterdir() if p.is_dir()):
        files = sorted((d / "fab").glob(f"poem_{d.name}_BOM.*"))
        if files:
            boards[d.name] = read_bom(files[0])

    wb = Workbook()
    summary = wb.active
    summary.title = "集計"
    names = list(boards)
    summary.append(["部品", "メーカー", "LCSC", *names, "合計", "リンク"])

    agg = {}
    for b, items in boards.items():
        for d in items:
            a = agg.setdefault(part_key(d), {"d": d, "q": {}})
            a["q"][b] = a["q"].get(b, 0) + d["qty"]
    for key in sorted(agg, key=lambda k: agg[k]["d"].get("part", "").upper()):
        a = agg[key]
        d, q = a["d"], [a["q"].get(b, "") for b in names]
        summary.append([d.get("part", ""), d.get("maker", ""), d.get("lcsc", ""), *q,
                        sum(x for x in q if x != ""), d.get("link", "")])

    for b, items in boards.items():
        ws = wb.create_sheet(b[:31])
        ws.append([h for _, h in COLUMNS])
        for d in items:
            ws.append([d.get(k, "") for k, _ in COLUMNS])

    for ws in wb.worksheets:
        for c in ws[1]:
            c.font = Font(bold=True)
        ws.freeze_panes = "A2"
        autosize(ws)
    wb.save(OUT)
    print(f"{OUT.name}: {len(boards)} 基板, {len(agg)} 品目")


if __name__ == "__main__":
    main()
