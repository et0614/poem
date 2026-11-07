```{=openxml}
<w:p><w:r><w:br w:type="page"/></w:r></w:p>
```

::: {custom-style="Part Title"}
第4部 MQTT の電文
:::

> 元資料: OSL共通レジスタv2_アドレス表.md §12、firmware/poem_pod.X/main.c・node_manager.c、software/src/PoEMServer/NodePayloads.cs。

# トピックの体系

> 執筆予定: poem/<pod>/pod/{info,diag}、poem/<pod>/node/<adr>/{config,data,diag}。Pod ID (MAC 12 桁)、アドレスは 10 進。ほかの MQTT クライアントが購読する場合の約束。

# 電文の形式

## pod/info

> 執筆予定: fw、name、ip。

## pod/diag

> 執筆予定: Pod の I²C 診断の項目。

## node/config

> 執筆予定: uid (32 文字)、vendor、model、fw、period、name、ch[] (n、u、t、p、f)。

## node/data

> 執筆予定: sq、st[]、v[]。読めないときの形。値の書式 (整数型、有効桁数)。追加予定の短い ID (未決)。

## node/diag

> 執筆予定: up、rc、err、serr。

## 子機の一覧 (追加予定)

> 執筆予定 (未決): 全走査の後に見つかったアドレスの一覧を送り、サーバが撤去したセンサのオブジェクトを消せるようにする。

# 配送の約束

> 執筆予定: QoS 0。送る時機。config が失われたときの回復 (retain、定期的な再送、data の短い ID による照合)。未決の項目は付録 F。

# 時刻と順序

> 執筆予定: 時刻はサーバの受信時刻。sq の意味と、上位での使い方。
