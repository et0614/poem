# PoEM Pod 基板

親機。PoE 受電、Ethernet (W5500)、I²C 子機ゲートウェイ (AVR128DB32)。ファームウェアは `firmware/poem_pod.X`。

| ファイル | 内容 |
|---|---|
| `poem_pod.f3z` | 設計の元データ (Fusion) |
| `poem_pod_schematic.pdf` | 回路図 |
| `fab/poem_pod_gerber.zip` | ガーバー |
| `fab/poem_pod_BOM.csv` | BOM |
| `fab/poem_pod_CPL.csv` | 部品配置 (JLCPCB で実装するとき) |
| `poem_pod_3d.stp` | 3 次元のモデル (STEP。ケースの設計に使う) |
| `enclosure/` | ケース (`loggercase.3dm` が元データ、`cap.stp`・`bottom.stp` が部品) |
