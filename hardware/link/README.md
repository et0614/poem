# PoEM Link 基板

USB-MIDI ⇔ I²C ブリッジ (AVR32DU20)。ファームウェアは `firmware/poem_link.X`、通信仕様は仕様書の 6.2 節 (`docs/spec/src/60_link.md`)。

| ファイル | 内容 |
|---|---|
| `poem_link_schematic.pdf` | 回路図 |
| `fab/poem_link_gerber.zip` | ガーバー (JLCPCB にそのまま上げる) |
| `fab/poem_link_BOM.csv` | 実装用 BOM (JLCPCB 形式) |
| `fab/poem_link_CPL.csv` | 実装用 部品配置 (JLCPCB 形式) |
| `poem_link_3d.png` | 3 次元の姿図 |

設計の元データ (`poem_link.f3z`) はまだ入っていない。

## BOM の部品

| 部品 | 値・品番 | LCSC | 種別 | 備考 |
|---|---|---|---|---|
| C1, C2 | 1µF 0402 | C52923 | Basic | LDO の入出力 |
| C3, C4 | 100nF 0402 | C1525 | Basic | VDD・VUSB のパスコン |
| D1 | 赤 LED 0603 (KT-0603R) | C2286 | Basic | 回路図の SML-D12D1WT86 (橙) は JLCPCB の在庫が数個しかないため置き換え |
| R1 | 1kΩ 0402 | C11702 | Basic | LED の電流 約 1.3 mA |
| RN1, RN2 | 10kΩ×4 アレイ 4D02WGJ0103TCE (UNI-ROYAL) | C25725 | Preferred | 2 本並列で約 5kΩ (CC プルダウン、I²C プルアップ)。0402×4 のアレイは全基板でこの品にそろえる。初回の発注 (2026-10) は EXB28V103JX (Panasonic、C178757) |
| J1 | TYPE-C-31-M-14 | C223907 | Extended | |
| J2 | SM04B-SRSS-TB (Qwiic) | C160404 | Extended | 安価な互換品 XY-SM04B-SRSS-TB (C51940130) もある |
| U1 | XC6206P332MR-G | C5446 | Basic | |
| U2 | AVR32DU20-I/REB | C37548385 | Extended | **JLCPCB の在庫なし (2026-10-23 時点)**。事前に部品を手配する必要がある |
| U3 | USBLC6-2SC6 (TECH PUBLIC) | C2827654 | Extended | ライブラリ指定の品番。ST 純正は C7519 |

- UPDI の書き込みパッド (U$3、裏面) は部品が無いので BOM・CPL から除いた。
- C1〜C4 と R1 の値は回路図に記載が無いので、検討時の値 (C1・C2 = 1µF、C3・C4 = 100nF) と上記の LED 抵抗で埋めた。
- CPL の回転角は Fusion の値のまま。発注画面のプレビューで、特に U2 (1 ピンの位置)・U1・U3・D1 (極性)・J1 の向きを確認すること。
