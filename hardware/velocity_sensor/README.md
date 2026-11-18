# 風速センサ 基板

定温度型の熱線風速計 (熱線は白金測温体 Pt100、温度補償は Ni 薄膜 PTC)。ファームウェアは `firmware/poem_velocity_sensor.X`。

風速プローブ v5 (`v5_BOM案.xlsx` の「風速プローブ」シート) を元に、次を変えたもの。

- コネクタ J1 を 2 mm ピッチのピンヘッダ (X4611WRS-04I-C40D28) から Qwiic (SM04B-SRSS-TB) に変えた。
- 状態の表示に LED (D1) と電流制限抵抗 (R12、1 kΩ) を加えた。

| ファイル | 内容 |
|---|---|
| `poem_velocity_sensor.f3z` | 設計の元データ (Fusion) |
| `poem_velocity_sensor_schematic.pdf` | 回路図 |
| `poem_velocity_sensor_3d.png` | 3 次元の姿図 |
| `fab/poem_velocity_sensor_gerber.zip` | ガーバー (Fusion の CAM 出力。JLCPCB にそのまま上げる) |
| `fab/poem_velocity_sensor_BOM.csv` | 実装用 BOM (JLCPCB 形式) |
| `fab/poem_velocity_sensor_CPL.csv` | 実装用 部品配置 (JLCPCB 形式) |

基板の大きさは 58.5 × 8 mm、2 層。

## 部品

部品の選定の理由と代替案は `v5_BOM案.xlsx` の「風速プローブ」シートにある。ここでは発注に関わることだけ書く。

| 部品 | 値・品番 | LCSC | 調達 | 備考 |
|---|---|---|---|---|
| U1 | TPS61099DRVR | C2842395 | JLC 在庫 | 昇圧 (熱線の電源、約 4.3 V) |
| L1 | CKCS201610-2.2uH/M | C354533 | JLC 在庫 | |
| C1 | 10 µF 0603 | C19702 | JLC 在庫 | |
| C2, C3 | 22 µF 6.3 V 0603 | C59461 | JLC 在庫 | |
| R1 | 330 kΩ 0402 | C25778 | JLC 在庫 | 帰還抵抗 (出力 約 4.3 V) |
| R2 | 100 kΩ 0402 | C25741 | JLC 在庫 | |
| R3, R5, R10, R11 | 10 kΩ 0402 | C25744 | JLC 在庫 | |
| R4 | 1 kΩ 0402 | C11702 | JLC 在庫 | |
| Q1 | MMBT3906 | C2143 | JLC 在庫 | |
| R6 | 2.21 Ω 0.1% 25 ppm 0603 (CPF0603B2R21E1) | C3948871 | **JLC 取り寄せ** | 在庫 0 (2026-10-11)。間に合わなければ ARG03BTC2R20 (C2683027、2.2 Ω) |
| R7 | 232 Ω 0.1% 0402 | C852653 | JLC 在庫 | |
| R8 | 2.74 kΩ 0.1% 50 ppm 0402 (RT0402BRE072K74L) | C705647 | JLC 在庫 | v5 の BOM 案による。Fusion の部品属性 (ARG02BTC2741、C2681598) ではなくこちらを使う |
| RT2 | TFPT1206L1002FV | C144872 | **JLC 取り寄せ** | 在庫 0。間に合わなければ購入して支給 |
| U2 | COS8551TRA | C695778 | JLC 在庫 | |
| C5, C7, C8, C9 | 100 nF 0402 | C1525 | JLC 在庫 | |
| C6 | 1 µF 0402 | C52923 | JLC 在庫 | |
| U3 | AVR128DB32T-I/RXB | — | **支給** | 手持ち在庫を JLC へ送る (Parts Consignment) |
| J1 | SM04B-SRSS-TB (Qwiic) | C160404 | JLC 在庫 | PoEM Link と同じ |
| D1 | 赤 LED 0603 (KT-0603R) | C2286 | JLC 在庫 | 回路図の SML-D12D1WT86 を PoEM Link と同じ品に置き換えた |
| R12 | 1 kΩ 0402 | C11702 | JLC 在庫 | LED の電流 約 1.3 mA。R4・PoEM Link と同じ品 |

- 実装しない部品 (DNP): C4 (位相補償、値は試験で決める)、R9 (起動補助 1 MΩ)。BOM・CPL から除いた。
- RT1 (熱線、Pt100 の 5185645) は基板の先の穴に挿すリード部品で、JLC には登録が無い。BOM・CPL から除き、手で付ける。
- テストピン (GND, UPDI, VDD) は部品が無いので BOM・CPL から除いた。
- CPL の回転角は Fusion の値のまま。発注画面のプレビューで、特に U1・U2・U3 (1 ピンの位置)、Q1、D1 (極性)、J1 の向きを確認すること。
