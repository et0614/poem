# 温湿度センサ 基板

SHT41 で温度と湿度を測る。ファームウェアは `firmware/poem_temperature_sensor.X`。

| ファイル | 内容 |
|---|---|
| `poem_temperature_sensor.f3z` | 設計の元データ (Fusion) |
| `poem_temperature_sensor_schematic.pdf` | 回路図 |
| `poem_temperature_sensor_3d.png` | 3 次元の姿図 |
| `fab/poem_temperature_sensor_gerber.zip` | ガーバー (Fusion の CAM 出力。JLCPCB にそのまま上げる) |
| `fab/poem_temperature_sensor_BOM.csv` | 実装用 BOM (JLCPCB 形式) |
| `fab/poem_temperature_sensor_CPL.csv` | 実装用 部品配置 (JLCPCB 形式) |

基板の大きさは 7.4 × 30 mm、2 層。

## 部品

| 部品 | 値・品番 | LCSC | 備考 |
|---|---|---|---|
| U1 | AVR128DB32T-I/RXB | — | **支給**。手持ち在庫を JLC へ送る (Parts Consignment)。回路図・Fusion の部品は AVR32DB32 のままで、ピン配置とパッケージ (VQFN-32 5x5) は同じ。ファームウェアも AVR128DB32 向け |
| U2 | SHT41-AD1B-R2 | C7461861 | I²C アドレス 0x44 |
| C1, C2 | 100 nF 0402 | C1525 | C1: U1、C2: U2 |
| RN1 | 10 kΩ × 4 アレイ 0402 (4D02WGJ0103TCE) | C25725 | U2 との I²C (SDA_M/SCL_M) のプルアップ。回路図の EXB-28V103JX (Panasonic) と同じ寸法の UNI-ROYAL 品。0402×4 のアレイは全基板でこの品にそろえる。PoEM Pod 側の I²C (J1) にはプルアップを置かない |
| J1 | SM04B-SRSS-TB (Qwiic) | C160404 | PoEM Link と同じ |
| D1 | 赤 LED 0603 (KT-0603R) | C2286 | 回路図の SML-D12D1WT86 を PoEM Link と同じ品に置き換えた |
| R1 | 510 Ω 0603 | C23193 | LED の電流 約 2.5 mA |

- 裏面の UPDI の書き込みパッド (U$1) は部品が無いので BOM・CPL から除いた。
- CPL の回転角は Fusion の値のまま。発注画面のプレビューで、U1・U2 (1 ピンの位置)、D1 (極性)、J1 の向きを確認すること。
