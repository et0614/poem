"""PoEM のマンボウの SVG を、1 つの形の定義から全部作る。

マンボウの形を直したら、ここを直して実行し直す (生成物は手で編集しない)。

    python hardware/logo/make_mola.py

作るもの:
- hardware/logo/poem_mola_silk.svg            基板のシルク用 (1 色、幅 8 mm)
- PoEMLink.App/Resources/AppIcon/appiconfg.svg アプリのアイコンの前景 (白、456 角のキャンバス)
- PoEMLink.App/Resources/Splash/splash.svg     起動画面 (アイコンの前景と同じ)
- PoEMLink.App/Resources/Images/mola.svg       記録欄のマンボウ (白、口を開けた形)
- PoEMLink.App/Resources/Images/mola_closed.svg 同 (口を閉じた形)。MolaIndicator が 2 枚を切り替える

形: 基板の形をしたマンボウ。体は楕円、背びれと尻びれ、四角い目 (マイコン) と目の光、
口の切り欠き (コネクタ)。目と口は抜き (fill-rule evenodd) なので、下地の色が透けて見える。
座標はアイコンの設計の単位で、体の外形は x 26..146, y 13..171。
"""
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "software/src/PoEMLink.App/Resources"

CX, CY, RX, RY = 86, 92, 60, 44   # 体 (楕円)
BOX = (26, 13, 120, 158)          # 外形 (x, y, 幅, 高さ)


def _hit(p, q):
    """p (楕円の外) から q (楕円の中) へ進む線分が楕円と交わる点"""
    f = lambda t: ((p[0] + (q[0] - p[0]) * t - CX) / RX) ** 2 + ((p[1] + (q[1] - p[1]) * t - CY) / RY) ** 2 - 1
    a, b = 0.0, 1.0
    for _ in range(60):
        m = (a + b) / 2
        a, b = (m, b) if f(m) > 0 else (a, m)
    return (p[0] + (q[0] - p[0]) * a, p[1] + (q[1] - p[1]) * a)


def _n(v):
    return f"{v:.2f}".rstrip("0").rstrip(".")


def mola_path(mouth_open=True):
    """マンボウの輪郭・目・目の光を 1 本のパスにする (evenodd で目が抜け、光が残る)"""
    p1 = _hit((112, 18), (96, 54))    # 背びれの前の辺が体に入る点
    p2 = _hit((138, 62), (96, 54))    # 背びれの付け根の辺が体に入る点
    p3, p4 = (p2[0], 2 * CY - p2[1]), (p1[0], 2 * CY - p1[1])   # 尻びれは上下の対称
    pt = lambda p: f"{_n(p[0])} {_n(p[1])}"
    arc = f"A{RX} {RY} 0"
    d = (f"M{pt(p1)} L112 18 Q121 8 130 18 L138 62 L{pt(p2)} {arc} 0 1 {pt(p3)} "
         f"L138 122 L130 166 Q121 176 112 166 L{pt(p4)} ")
    if mouth_open:
        xm = CX - RX * math.sqrt(1 - ((80 - CY) / RY) ** 2)   # 口の切り欠きの上下の端が楕円に当たる x
        d += f"{arc} 0 1 {_n(xm)} 104 L36 104 L36 80 L{_n(xm)} 80 {arc} 0 1 {pt(p1)} Z"
    else:
        d += f"{arc} 1 1 {pt(p1)} Z"
    eye = "M51 66 H75 A3 3 0 0 1 78 69 V93 A3 3 0 0 1 75 96 H51 A3 3 0 0 1 48 93 V69 A3 3 0 0 1 51 66 Z"
    glint = "M54 72 H64 V82 H54 Z"
    return f"{d} {eye} {glint}"


HEAD = '<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
NOTE = "<!-- hardware/logo/make_mola.py で生成。手で編集しない -->\n"


def tight(fill, width, height, mouth_open=True, comment=""):
    """外形にぴったりのキャンバス"""
    x, y, w, h = BOX
    return (HEAD + NOTE + comment +
            f'<svg width="{width}" height="{height}" viewBox="{x} {y} {w} {h}" version="1.1" xmlns="http://www.w3.org/2000/svg">\n'
            f'    <path fill="{fill}" fill-rule="evenodd" d="{mola_path(mouth_open)}" />\n</svg>\n')


def icon_foreground():
    """アイコンの前景: 456 角のキャンバスに、体の高さをいっぱいに置く (余白は csproj の ForegroundScale で取る)"""
    s = 456 / BOX[3]
    cx, cy = BOX[0] + BOX[2] / 2, BOX[1] + BOX[3] / 2
    return (HEAD + NOTE +
            "<!-- PoEM のマンボウ: 基板の形をしたマンボウ。四角い目はマイコン、口の切り欠きはコネクタ。背景色は csproj の Color (#1565C0) -->\n"
            '<svg width="456" height="456" viewBox="0 0 456 456" version="1.1" xmlns="http://www.w3.org/2000/svg">\n'
            f'    <path transform="translate(228 228) scale({s:.4f}) translate({_n(-cx)} {_n(-cy)})" fill="#FFFFFF" fill-rule="evenodd" d="{mola_path()}" />\n'
            "</svg>\n")


def main():
    silk_w = 8.0
    silk_h = silk_w * BOX[3] / BOX[2]
    out = {
        ROOT / "hardware/logo/poem_mola_silk.svg": tight(
            "#000000", f"{silk_w:g}mm", f"{silk_h:.2f}mm",
            comment=f"<!-- PoEM のマンボウ (シルク用、1 色)。青い基板に白いシルクで刷る。目と口は抜き。"
                    f"幅 {silk_w:g} mm で、いちばん細い所 (目の光のまわり) が 0.4 mm -->\n"),
        APP / "AppIcon/appiconfg.svg": icon_foreground(),
        APP / "Splash/splash.svg": icon_foreground(),
        # 記録欄: 高さ 24 で描く (MauiImage はこの大きさを基準に画面の密度ごとの PNG を作る)
        APP / "Images/mola.svg": tight("#FFFFFF", 19, 24, True),
        APP / "Images/mola_closed.svg": tight("#FFFFFF", 19, 24, False),
    }
    for path, text in out.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
        print(path.relative_to(ROOT))


if __name__ == "__main__":
    main()
