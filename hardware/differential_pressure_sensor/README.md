# 差圧センサ 基板

SDP810-500PA (±500 Pa) で差圧を測る。測定範囲の狭い SDP810-125PA (±125 Pa) を載せる版もある (下記)。ファームウェアは `firmware/poem_differential_pressure_sensor.X`。

| ファイル | 内容 |
|---|---|
| `poem_differential_pressure_sensor.f3z` | 設計の元データ (Fusion) |
| `poem_differential_pressure_sensor_schematic.pdf` | 回路図 |
| `poem_differential_pressure_sensor_3d_1.png`、`_3d_2.png` | 3 次元の姿図 (表、裏) |
| `fab/poem_differential_pressure_sensor_gerber.zip` | ガーバー (Fusion の CAM 出力。JLCPCB にそのまま上げる) |
| `fab/poem_differential_pressure_sensor_BOM.csv` | 実装用 BOM (JLCPCB 形式) |
| `fab/poem_differential_pressure_sensor_CPL.csv` | 実装用 部品配置 (JLCPCB 形式) |

基板の大きさは 31 × 20 mm、2 層。

## 部品

| 部品 | 値・品番 | LCSC | 備考 |
|---|---|---|---|
| U1 | AVR128DB32T-I/RXB | — | **支給**。手持ち在庫を JLC へ送る (Parts Consignment)。回路図・Fusion の部品は AVR32DB32 のままで、ピン配置とパッケージ (VQFN-32 5x5) は同じ |
| U2 | SDP810-500PA | C153056 | 裏面の挿入部品 (下記) |
| C1, C2 | 100 nF 0402 | C1525 | |
| RN1 | 10 kΩ × 4 アレイ 0402 (4D02WGJ0103TCE) | C25725 | U2 との I²C のプルアップ。PoEM Pod 側の I²C (J1) にはプルアップを置かない。回路図の EXB-28V103JX と同じ寸法の UNI-ROYAL 品 |
| J1 | SM04B-SRSS-TB (Qwiic) | C160404 | |
| D1 | 赤 LED 0603 (KT-0603R) | C2286 | 回路図の SML-D12D1WT86 を置き換えた |
| R1 | 1 kΩ 0402 | C11702 | LED の電流 約 1.3 mA。PoEM Link と同じ品 |

- U2 (SDP810) はピンを挿して付ける部品で、裏面にある。BOM・CPL には入れてあり、JLC で実装する前提とする。JLC に頼むかどうかは発注のたびに費用を見て決め、高ければ BOM・CPL から U2 の行を消して、届いてから手で付ける。リフローには使えない部品と考えられる (要確認) ので、JLC では挿入部品の実装 (手はんだ・ウェーブ) になる。
- CPL の U2 の座標は、Fusion の裏面の PnP の値をそのまま使った。発注画面のプレビューで、裏面の位置と向きが合っているか確認すること。
- **SDP810-125PA の版**: U2 を SDP810-125PA (±125 Pa) に替えた版もある。外形とピン配置は 500PA と同じなので、基板・CPL は共通で、BOM の U2 の行 (Comment と LCSC の品番) だけを替える。125PA の LCSC の品番は発注のときに確認する。
- 裏面の UPDI の書き込みパッド (U$1) は部品が無いので BOM・CPL から除いた。
- CPL の回転角は Fusion の値のまま。発注画面のプレビューで、U1 (1 ピンの位置)、D1 (極性)、J1 の向きを確認すること。
