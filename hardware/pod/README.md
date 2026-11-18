# PoEM Pod 基板

親機。PoE で受電し (Si3404)、Ethernet (W5500) と I²C の子機をつなぐ (AVR128DB32)。ファームウェアは `firmware/poem_pod.X`。Fusion 上のボード名は PoEMPod。

| ファイル | 内容 |
|---|---|
| `poem_pod.f3z` | 設計の元データ (Fusion) |
| `poem_pod_schematic.pdf` | 回路図 |
| `poem_pod_3d.png` | 3 次元の姿図 |
| `poem_pod_3d.stp` | 3 次元のモデル (STEP。ケースの設計に使う。2026-11-18 の部品の変更より前の版) |
| `fab/poem_pod_gerber.zip` | ガーバー (Fusion の CAM 出力。JLCPCB にそのまま上げる) |
| `fab/poem_pod_BOM.xlsx` | **部品表の正本** (メーカー品番と Digi-Key のリンク) |
| `fab/poem_pod_BOM.csv` | 実装用 BOM (JLCPCB 形式)。部品は `poem_pod_parts_plan.md` の案 |
| `poem_pod_parts_plan.md` | JLCPCB 実装に向けた部品の置き換え案と、その理由 |
| `lib/` | Fusion の部品と STEP (J301 の HR873168AE。不採用の HR871150C も残す) |
| `fab/poem_pod_CPL.csv` | 実装用 部品配置 (JLCPCB 形式) |
| `enclosure/` | ケース (`loggercase.3dm` が元データ、`cap.stp`・`bottom.stp` が部品) |

基板の大きさは 80 × 35 mm、2 層。

## 部品表について

- 部品の値は `poem_pod_BOM.xlsx` が正しい。回路図・Fusion の部品の値には古いものが残っている (例: C102、C104、C108・C112・C113、C208・C209、C305・C306、R110)。
- 2026-11-18 に、JLCPCB で安く実装できるよう部品を置き換えた (汎用の R・C を 0402 や Basic 品に、J301 を HR873168AE に、L103 を CDRH127/LDNP-101MC に、C103 を SMD のアルミ電解に、SW801・SW802 を TS-1088 に)。置き換えた部品と理由は `poem_pod_parts_plan.md`。xlsx はまだ旧品番のまま。
- `poem_pod_BOM.csv` は、この案の LCSC 品番で作った。D801 は M-Logger と同じ KT-0603G (C12624)。SW201 (スライドスイッチ) は LCSC の品番が未定なので、旧品番のまま空いている。
- IC801 (AVR128DB32) は手持ち在庫を支給する。Fusion の部品は AVR32DB32 のまま。
- LDO の部品番号は、xlsx では IC201、Fusion では I201。CSV は Fusion に合わせて I201 とした。
- 書き込み用のパッド (UPDI) と配線用のパッド (PD1・PD4) は、BOM・CPL から除いた。
- C103 は極性がある。シルクの + の側 (D111 の側) が VPOS。
- CPL の回転角は Fusion の値のまま。発注画面のプレビューで、IC・ダイオードの極性・C103・LED・コネクタの向きを確認すること。
