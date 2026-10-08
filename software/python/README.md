# 子機読み出しツール (MCP2221 / CP2112)

親機 (PoEM Pod) を使わずに、Windows PC から USB-I2C ブリッジで子機を直接読むための Python スクリプト群。
子機単体の動作確認、I2C アドレス・名前の確認、計測値の記録に使う。

対応する子機 (OSL 共通レジスタ、SHASE 2026 第2報):

| 子機 | ファームウェア | value[0] | value[1] |
|---|---|---|---|
| 温湿度センサ (SHT4x) | `poem_temperature_sensor.X` | 温度 [°C] | 相対湿度 [%RH] |
| 風速センサ | `poem_velocity_sensor.X` | 風速 [m/s] | 回路電圧 [V] |
| 絶対圧センサ (BMP585) | `poem_absolute_pressure_sensor.X` | 気圧 [Pa] | センサ温度 [°C] |
| 差圧センサ (SDP810-125Pa) | `poem_differential_pressure_sensor.X` | 差圧 [Pa] | センサ温度 [°C] |

## 準備

```
pip install hidapi
pip install matplotlib      # グラフ表示 (--plot) を使うときだけ
```

- MCP2221 / CP2112 はどちらも Windows の標準 HID として認識されるので、ドライバのインストールは要らない。
- 子機の SDA / SCL / GND をブリッジにつなぎ、子機に電源を入れる。I2C のプルアップ抵抗がバス上のどこかに必要。

## osl_reader.py — 子機の情報を読む

```
cd software/python
python osl_reader.py                            バスを走査し、見つかった子機をすべて表示
python osl_reader.py --addr 0x10                指定アドレスの子機だけ表示
python osl_reader.py --watch 1                  1 秒ごとに計測値を表示し、CSV にも書き出す
python osl_reader.py --watch 1 --csv log.csv    CSV のファイル名を指定する (子機ごとに log_0xNN.csv)
python osl_reader.py --plot                     計測値をグラフでリアルタイム表示する (1 秒ごと)
python osl_reader.py --plot --span 3600         グラフの横軸を直近 1 時間にする (既定 600 秒)
python osl_reader.py --bridge cp2112            CP2112 に固定する
python osl_reader.py --bridge mcp2221           MCP2221 に固定する
python osl_reader.py --serial XXXX              ブリッジが複数あるときにシリアル番号で選ぶ
```

`--watch` は Ctrl+C で終了する。

ブリッジは自動で選ぶので、MCP2221 と CP2112 のどちらをつないでも同じコマンドで動く。
MCP2221 → CP2112 の順に探して最初に見つかったものを使い、使ったものを
`Bridge: MCP2221 - ...` のように表示する。両方つないでいるときは MCP2221 になるので、
CP2112 を使いたければ `--bridge cp2112` を付ける。

### 表示例

```
[0x10] 差圧センサ (SDP810)
  Device ID : 3688892 (0x3849BC)
  Name      : ""
  Data count: 2  units=[53, 62]
  Status1   : 0x00
  meas_count: 143   err_count: 0
  Values    : -0.02 Pa, 21.79 °C
  補正係数  : 差圧 y = 1 x + 0
              温度 y = 1 x + 0
```

| 項目 | 意味 |
|---|---|
| Device ID | 子機マイコンのシリアル番号から作った 22 bit の個体識別番号。親機の MQTT トピック `nodes/data/<id>` と同じ値 |
| Name | 子機の EEPROM に保存された装置ラベル (最大 15 文字) |
| units | 各計測値の BACnet 単位コード (53 = Pa, 62 = °C, 29 = %RH, 74 = m/s, 5 = V) |
| Status1 | bit i が立っていれば value[i] は無効 (stale)。0xFF は全計測値が無効 (起動直後の 1 秒間やセンサ通信失敗) |
| meas_count | 子機が数えた計測回数。125 ms ごとに +1 (1 秒で 8)、255 の次は 0。再起動すると 0 に戻る |
| err_count | 子機が検出した I2C 異常の回数 (255 で止まる) |
| 補正係数 | 拡張領域 0x4C- の線形補正 `y = a x + b`。風速センサは形式が違うので表示しない |

絶対圧センサと差圧センサはレジスタも単位も同じなので、value[0] の大きさで見分けている
(10 kPa 以上なら絶対圧)。値が無効なときは「圧力センサ」とだけ表示する。

### watch 表示

```
23:03:49 [0x10] st=0x00 Δsq=  8  -0.04 Pa, 21.67 °C
```

計測値は小数点以下 2 桁で表示する (グラフの最新値・目盛・ホバー表示も同じ)。
CSV には丸めずに float の精度いっぱいで書く。桁数は `osl_reader.py` の `VALUE_DECIMALS` で変えられる。

`Δsq` は前回からの meas_count の進み。正常なら 1 秒間隔で 8 前後になる。
PC と子機の時計がわずかにずれているため、ときどき 7 や 9 が出るのは正常。
大きく外れたり 0 が続いたりする場合は、子機の再起動や停止を疑う。

### CSV

`--watch` 中は毎回の読み出しを CSV に追記する。**子機ごとに別のファイル**にし、ファイル名の
末尾に子機のアドレスを付ける。ファイル名を指定しなければ、実行したフォルダに
`osl_YYYYMMDD_HHMMSS_0x11.csv`、`osl_YYYYMMDD_HHMMSS_0x45.csv` のように作る。
`--csv log.csv` と指定すると `log_0x11.csv`、`log_0x45.csv` になる。毎周期ディスクへ書き出すので、途中で止めてもそこまでのデータは残る。
BOM 付き UTF-8 なので Excel でそのまま開ける。

```
datetime,addr,device_id,name,sensor,result,status1,meas_count,delta_sq,err_count,value0,unit0,value1,unit1
2026-10-06T23:03:49.251,0x10,3688892,,差圧センサ (SDP810),ok,0,120,8,0,-0.04270833,Pa,21.66625,°C
```

- 1 行が 1 回の読み出し。どの子機も同じ時刻に読むので、ファイル間で `datetime` が揃う。
- `value_i` と `unit_i` が対になっている。列の数は子機の計測値の数で決まる。値は float の精度いっぱい (7 桁) で書く。
- 読み出しに失敗した回は `result` が `read_failed` の行になり、値の列は空になる。
- 値が有効かどうかは `status1` の bit i で判断する (0 なら全計測値が有効)。

### グラフ (--plot)

`--plot` を付けると、watch と同じ読み出しをしながらグラフのウィンドウを開く。
コンソール表示と CSV 書き出しも同時に行う。ウィンドウを閉じると終了する。
`--watch` を省略したときは 1 秒周期になる。

- 子機ごとに 1 行、計測値ごとに 1 列のグラフを並べる。単位の違う値 (気圧と温度など) は
  別のグラフに分け、縦軸を 2 本にはしない。
- 各グラフの左上に子機と計測値の名前、右上に最新値を出す。
- 横軸は直近 `--span` 秒。縦軸は表示中の範囲の値に合わせて自動で伸び縮みする。
- stale の値と読み出し失敗は線が途切れる。右上の最新値も「無効」「読み出し失敗」になる。
- マウスを重ねると、その時刻の値を表示する。拡大・移動はウィンドウ下のツールバーで行う。
- 描画のコードは `osl_plot.py` にある。

## osl_set_address.py — I2C アドレスを変更する

```
python osl_set_address.py 0x10 0x11          0x10 の子機を 0x11 に変える (確認あり)
python osl_set_address.py 0x10 0x11 --yes    確認せずに変える
```

`--bridge` と `--serial` は osl_reader.py と同じ。

1. 現在のアドレスにいる子機の機種・Device ID・名前を表示する。
2. 新しいアドレスに別の機器がいないか確かめる (いれば中止)。
3. 確認のあと、鍵 0xA5 と新アドレスを 1 回の書き込みで送る。
4. 新しいアドレスで同じ Device ID の子機が応答することを確かめる。

子機は書き込みを受けた時点で EEPROM に保存し、そのまま新しいアドレスに切り替わる。
リセットや電源の入れ直しは要らない。新しいアドレスは 0x08-0x77 の範囲で選ぶ。

- PoEMServer の BACnet オブジェクト番号は I2C アドレス × 100 なので、アドレスを変えると
  その子機の番号も変わる (0x10 → 0x11 なら 1600- → 1700-)。
- 親機につないだまま変えた場合、新しいアドレスは 10 秒以内に新しい子機として見つかるが、
  古いアドレスは応答なしのまま一覧に残る。親機の HTTP 設定画面で SCAN をやり直すと消える。

## mcp2221_driver.py — MCP2221 ドライバ

`cp2112_driver.py` の `CP2112Device` と同じ関数を持つ (`read_i2c_block` / `write_i2c_block` /
`open` / `close` / `with` 文 / `list_devices`)。`temperature_manager.py` などの既存スクリプトも、
基底クラスを `MCP2221Device` に差し替えれば MCP2221 で動く。

```python
from mcp2221_driver import MCP2221Device

with MCP2221Device(slave_addr=0x10) as dev:
    raw = dev.read_i2c_block(0x28, 36)       # POLL BLOCK を読む。失敗なら None
    dev.write_i2c_block(0x18, list(b'ROOF') + [0] * 12)   # 名前を書く
```

単体で実行すると、接続中の MCP2221 の一覧とファームウェア版数、SCL/SDA の状態を表示する。

```
python mcp2221_driver.py
```

## 困ったとき

| 症状 | 確認すること |
|---|---|
| `No MCP2221 / CP2112 devices found.` | USB の接続。デバイスマネージャーで「USB 入力デバイス」として見えているか |
| `子機が見つかりません` | 配線、プルアップ抵抗、子機の電源。`python mcp2221_driver.py` で SCL / SDA が 1 になっているか |
| SCL または SDA が 0 | プルアップ抵抗が無いか、子機が SCL/SDA を Low に張り付かせている。子機の電源を入れ直す |
| 応答がおかしい・止まる | 他のソフト (MCP2221 Utility など) が同じ MCP2221 を開いていないか |

MCP2221 は、電源投入後に一度も I2C を使っていない状態で転送キャンセルを受けると、内部状態 0x62 に
落ちて戻らなくなる。ドライバはこれを避けているうえ、万一固まった場合はチップを自動でリセットする
(USB の再接続が起きるので 2 秒ほど待たされる)。

## その他のスクリプト (CP2112 用)

| ファイル | 内容 |
|---|---|
| `cp2112_driver.py` | CP2112 ドライバ |
| `i2c_scan.py` | I2C バスの応答アドレスを列挙 |
| `dump_memory.py` | 子機のレジスタを 16 進ダンプ |
| `temperature_manager.py` | 温湿度センサの補正係数・名前・アドレスの読み書き |
| `anemometer_manager.py` | 風速センサの設定の読み書き |
