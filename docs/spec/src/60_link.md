```{=openxml}
<w:p><w:r><w:br w:type="page"/></w:r></w:p>
```

# PoEM Link と補助ソフト

> 元資料: Drive の PoEM_Link_通信仕様.md、PoEM_Linkアプリ_方針.md、hardware/poem_link、firmware/poem_link.X、software/src/PoEMLink.*。

## PoEM Link のハードウェア

> 執筆予定: マイコン (AVR32DU20)、USB Type-C (USB-MIDI クラス準拠)、Qwiic、電源 (XC6206)、プルアップ、LED。接続できる機器 (iPhone・Android・Windows・Mac)。

## USB-MIDI の通信仕様

> 執筆予定: SysEx の形 (メーカー ID、製品コード、SEQ、コマンド)、8-to-7 符号化、コマンドの一覧と形式、結果コード、I²C の動作 (速度、タイムアウト、バス固着からの復帰)、LED の表示。メーカー ID の扱い (開発用 0x7D、正式な ID の取得)。

## PoEM Link アプリ

> 執筆予定: 位置づけ (PoEM の補助)、機能 (子機の表示、記録と CSV、名前とアドレスの変更、識別点滅、Link とバスの状態)、模擬モード、対応 OS、配布 (App Store・TestFlight)。

## 校正の手順

> 執筆予定: 係数の読み出しと書き換え、日付と履歴の記録、読み返しによる確認、Pod に戻した後の扱い。どの道具で行うか (アプリか PC の道具か) は未決 (付録 F)。

## PC 用の道具

> 執筆予定: software/tools/python の道具 (osl_reader、osl_set、osl_v2_test、dump_memory など) と、対応する USB-I²C 変換器 (CP2112、MCP2221、PoEM Link)。
