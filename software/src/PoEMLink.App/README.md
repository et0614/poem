# PoEM Link アプリ

PoEM Link (USB-MIDI ⇔ I²C ブリッジ) にスマホや PC をつなぎ、OSL v2 の子機を直接読む補助ソフト。
iOS・Android・Windows (.NET MAUI、net10)。方針は Drive の `PoEM_Linkアプリ_方針.md`、
通信仕様は `PoEM_Link_通信仕様.md`。

フォルダの構成とソリューションは `software/README.md` を参照。PoEM Link に関わるプロジェクト:

| プロジェクト | 内容 |
|---|---|
| `src/PoEM.Common` | サーバとアプリで共通の表 (機種名・単位・状態の名前) と Device ID (22bit) の計算 |
| `src/PoEMLink.Core` | MAUI に依存しない部分。Link のブリッジ層 (`Link/`)、OSL v2 の解釈 (`Osl/`)、記録と CSV (`Recording/`)、Link と子機の模擬 (`Simulation/`) |
| `tests/PoEMLink.Core.Tests` | 単体試験 (xUnit)。模擬の Link と子機を相手に、走査・POLL・名前とアドレスの変更・BATCH の分割まで確かめる |
| `src/PoEMLink.App` | MAUI アプリ (このフォルダ)。画面、OS ごとの MIDI (`Platforms/*/…LinkTransport.cs`、E-Sensor から移植)、記録中のバックグラウンド動作 |

## ビルドと試験

`software` フォルダで実行する。

```bash
dotnet test tests/PoEMLink.Core.Tests
```

```bash
dotnet build src/PoEMLink.App -f net10.0-windows10.0.19041.0
```

- Android は `-f net10.0-android`、iOS は `-f net10.0-ios` (署名と実機への配布は Mac が要る)。
- 実機なしで画面を確かめるには、アプリの「Link と設定」で模擬モードを入れる。
  Debug ビルドでは、環境変数 `POEMLINK_DEMO=1` を付けて起動すると最初から模擬モードになる。

## 作りの要点

- 表示は子機が名乗るチャネル記述 (名前・単位・桁数・状態) から組み立てる。機種ごとの作り込みはしない。
- 3 秒ごとにアドレスを走査し直し、子機の抜き差しとアドレスの変化を反映する。OSL と確認できない機器には書き込まない。
- POLL は子機ごとの更新周期 (下限 250 ms) で読み、同じ時機の子機は BATCH で 1 往復にまとめる。
- 記録の CSV は子機とチャネルごとに「値」と「状態」(BACnet Reliability の値。0 = 正常) の列を出す。値は丸めない。
