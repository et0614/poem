# PoEM Tap 基板

Qwiic の I²C を 3 方向に分け、スイッチでプルアップを入れ切りする (ファームウェアなし)。

| ファイル | 内容 |
|---|---|
| `poem_tap.f3z` | 設計の元データ (Fusion。ボード名は PoEM_Tap2) |
| `poem_tap_schematic.pdf` | 回路図 |
| `poem_tap_3d.png` | 3 次元の姿図 |
| `fab/poem_tap_gerber.zip` | ガーバー (Fusion の CAM 出力。JLCPCB にそのまま上げる) |
| `fab/poem_tap_BOM.csv` | 実装用 BOM (JLCPCB 形式) |
| `fab/poem_tap_CPL.csv` | 実装用 部品配置 (JLCPCB 形式) |

基板の大きさは 14 × 21 mm (T 字形)、2 層。部品はすべて表面。裏面のシルクにロゴと各コネクタの信号名がある。

## 部品

| 部品 | 値・品番 | LCSC | 備考 |
|---|---|---|---|
| SW1 | Nidec CAS-220TA (2 回路 2 接点のスライドスイッチ、J フック) | C2921538 | SDA・SCL のプルアップを 2 回路で同時に入れ切りする。シルクの表示は On / Off |
| RN1 | 10 kΩ × 4 アレイ 0402 (4D02WGJ0103TCE) | C25725 | プルアップ。10 kΩ を 2 本ずつ並列にし、SDA・SCL それぞれ 5 kΩ にする。回路図・Fusion の部品は EXB-28V103JX (Panasonic) で、同じ寸法の UNI-ROYAL 品にそろえる |
| J1, J2, J3 | SM04B-SRSS-TB (Qwiic) | C160404 | PoEM Link と同じ |

- CAS-220TA の Fusion の部品と 3D モデルは Ultra Librarian のもの。
- CPL の回転角は Fusion の値のまま。発注画面のプレビューで、SW1・RN1 と J1〜J3 の向きを確認すること。
