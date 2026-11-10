# ハードウェア

基板ごとにフォルダを分ける。リポジトリ全体が PoEM のものなので、フォルダ名に `poem_` は付けない。

| フォルダ | 基板 | 内容 | ファームウェア |
|---|---|---|---|
| [`pod/`](pod/) | PoEM Pod | 親機。PoE 受電、Ethernet (W5500)、I²C 子機ゲートウェイ (AVR128DB32) | `firmware/poem_pod.X` |
| [`link/`](link/) | PoEM Link | USB-MIDI ⇔ I²C ブリッジ (AVR32DU20) | `firmware/poem_link.X` |
| [`tap/`](tap/) | PoEM Tap | I²C の分岐・プルアップ (ファームウェアなし) | — |
| [`temperature_sensor/`](temperature_sensor/) | 温湿度センサ | SHT4x | `firmware/poem_temperature_sensor.X` |
| [`velocity_sensor/`](velocity_sensor/) | 風速センサ |  | `firmware/poem_velocity_sensor.X` |
| [`absolute_pressure_sensor/`](absolute_pressure_sensor/) | 絶対圧センサ | BMP585 | `firmware/poem_absolute_pressure_sensor.X` |
| [`differential_pressure_sensor/`](differential_pressure_sensor/) | 差圧センサ | SDP810 | `firmware/poem_differential_pressure_sensor.X` |

## 基板のフォルダの構成

```
xxx/
├─ README.md                版、部品の注記、発注時の注意
├─ poem_xxx.f3z             設計の元データ (Fusion)
├─ poem_xxx_schematic.pdf   回路図
├─ poem_xxx_3d.png          3 次元の姿図 (Fusion から書き出す。2 枚なら _3d_top.png・_3d_bottom.png)
├─ fab/                     基板の発注に使うもの
│   ├─ poem_xxx_gerber.zip
│   ├─ poem_xxx_BOM.csv     (または .xlsx)
│   └─ poem_xxx_CPL.csv     (JLCPCB で実装するとき)
└─ enclosure/               ケース (あるものだけ)
```

- ファイル名は `poem_<フォルダ名>_<種類>` とする。ファイルは発注や受け渡しでリポジトリの外に出るので、外でも何のファイルか分かるよう `poem_` を付ける。
- 版はフォルダで分けず、git の履歴と各 README の版の欄で管理する。

## 全基板の BOM

[`poem_bom.xlsx`](poem_bom.xlsx) は、各基板の `fab/*_BOM.*` から作る一覧である。シート「集計」は部品ごとの基板別の数量、ほかのシートは基板ごとの BOM。

- 生成物なので、手で編集しない。BOM を直したら、次を実行して作り直す。

  ```
  python hardware/build_bom.py
  ```

- 同じ部品は LCSC の品番 (無ければメーカー品番) でまとめる。基板によって書き方が違うと別の行になる。

## データシート

`datasheets/` は手元の参照用で、git に入れない。
