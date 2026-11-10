# PoEM Pod 基板

親機。PoE で受電し (Si3404)、Ethernet (W5500) と I²C の子機をつなぐ (AVR128DB32)。ファームウェアは `firmware/poem_pod.X`。Fusion 上のボード名は PoEMLogger。

| ファイル | 内容 |
|---|---|
| `poem_pod.f3z` | 設計の元データ (Fusion) |
| `poem_pod_schematic.pdf` | 回路図 (未作成) |
| `poem_pod_3d.png` | 3 次元の姿図 |
| `poem_pod_3d.stp` | 3 次元のモデル (STEP。ケースの設計に使う) |
| `fab/poem_pod_gerber.zip` | ガーバー (Fusion の CAM 出力。JLCPCB にそのまま上げる) |
| `fab/poem_pod_BOM.xlsx` | **部品表の正本** (メーカー品番と Digi-Key のリンク) |
| `fab/poem_pod_BOM.csv` | 実装用 BOM (JLCPCB 形式)。`poem_pod_BOM.xlsx` から作った |
| `fab/poem_pod_CPL.csv` | 実装用 部品配置 (JLCPCB 形式) |
| `enclosure/` | ケース (`loggercase.3dm` が元データ、`cap.stp`・`bottom.stp` が部品) |

基板の大きさは 80 × 35 mm、2 層。

## 部品表について

- 部品の値は `poem_pod_BOM.xlsx` が正しい。回路図・Fusion の部品の値には古いものが残っている (例: C102、C104、C208・C209、C305・C306、R104、R105、R108、R110)。
- `poem_pod_BOM.csv` の LCSC の品番は、RN1・RN2 と J101 のほかは空いている。JLCPCB の BOM の照合ではメーカー品番 (Comment) から部品を探す。在庫の無い部品は、発注のたびに代替品か取り寄せを決める。
- RN1、RN2 (10 kΩ × 4) は xlsx に無いので、全基板共通の UNI-ROYAL 品 (C25725) を入れた。
- IC801 (AVR128DB32) は手持ち在庫を支給する。Fusion の部品は AVR32DB32 のまま。
- LDO の部品番号は、xlsx では IC201、Fusion では I201。CSV は Fusion に合わせて I201 とした。
- 書き込み用のヘッダ (WRITE) と配線用のパッド (PD1〜PD4) は、BOM・CPL から除いた。

## 確認が要ること

- xlsx の 1 行目 (0.1 µF) の部品番号に C208・C209 があり、8 行目 (4.7 µF) と重なっていた。基板にある 0.1 µF は C101、C111、C201〜C206、C301、C302、C803 の 11 個なので、CSV ではそれに直した。xlsx の数量 16 と、4 行目の C402 (基板に無い) と数量 3 は、手配の予備かどうか要確認。
- 次の部品は、xlsx の品番が 0805 (2012) なのに、Fusion のフットプリントが 0603 になっている: C207 (GRM21BR61E106)、C208・C209 (GRM21BR61H475)、C303・C304 (GRM2195C2A682)。どちらかに合わせる必要がある。
- CPL の回転角は Fusion の値のまま。発注画面のプレビューで、IC・ダイオードの極性・LED・コネクタの向きを確認すること。
