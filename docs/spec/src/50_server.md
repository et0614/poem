```{=openxml}
<w:p><w:r><w:br w:type="page"/></w:r></w:p>
```

::: {custom-style="Part Title"}
第5部 PoEMServer と BACnet への公開
:::

> 元資料: software/src/PoEMServer (Program.cs、BACnet/PoEMDevice.cs、BACnet/PodRegistry.cs、setting.ini)、software/src/PoEM.Common。

# サーバの役割と設定

> 執筆予定: MQTT ブローカーと BACnet の公開を兼ねる。ネットワーク 2 本差し (mqttip と bacip)。setting.ini の各項目。複数のサーバの共存 (IP を分ける)。実行環境 (Windows・Linux、systemd)。

# BACnet Device

> 執筆予定: 1 サーバ = 1 Device。ポート 47808。Device 番号の既定 (bacip の末尾)。Device の名前。

# オブジェクトの番号の規則

> 執筆予定 (規定): Pod の範囲 (Pod の番号 × 12000)、Pod の情報 (pbase + 0〜)、子機の範囲 (pbase + アドレス × 100)、項目の配置。Device の中で、種類が違っても番号を重ねないこと (BMS が番号だけで点を区別するため)。PodMap.json による Pod の番号の固定。番号の上限 (Pod 349 台)。
>
> 未決: 項目の配置の見直し (付録 F)。

# オブジェクトの名前の規則

> 執筆予定 (規定): Object_Name の組み立て (場所、表示用 ID、利用者の名前、TSC コード、項目名)。長さと使える文字の制約。名前を変えたとき・センサを交換したときの扱い。Description に入れるもの。
>
> 未決: 形式の確定 (付録 F)。元資料: OSL v2 通信の整理 (Object_Name の案)。

# 表示用 ID

> 執筆予定: UID (16 バイト) から FNV-1a で 22 ビットに畳み、0x100000 を足して 16 進 6 桁で表す。人が見分けるためのもので、キーには使わない。

# 状態の反映

> 執筆予定: Reliability と STATUS_FLAGS の FAULT、異常の間の値の凍結、最終受信日時・最終正常日時。受信途絶の判定 (一定時間受信が無ければ状態 12 にする) (未決)。

# 構成の変化への追従

> 執筆予定: 子機の交換 (UID の変化でオブジェクトを作り直す)、アドレスの変更、Pod の移設、撤去したセンサの削除、名前の変更。

# TSC コードの管理

> 執筆予定: TSC コードの位置づけ (建築設備のネーミングコード、規定の名称と版)、登録・編集の方法、保存先、場所に付けるか個体に付けるか。
>
> 未決: 全体 (付録 F)。
