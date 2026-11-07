# PoEM Server と BACnet への公開

この章は、PoEM Server の機能と、BACnet への公開の規則を定める。

## PoEM Server の役割と設定

### 役割

PoEM Server は、次の 2 つの役割を 1 つのプログラムで持つ。

- MQTT ブローカー: PoEM Pod からの電文 (第 4 章) を受ける。ほかの MQTT クライアントにも配送する。
- BACnet の公開: 受けた電文を、BACnet/IP の 1 台の Device のオブジェクトとして公開する。

### ネットワーク

PoEM Server を動かす計算機は、PoEM Pod の側のネットワークと、BMS の側のネットワークに、別々につないでよい (2 本の LAN ケーブル)。MQTT は PoEM Pod の側の IP アドレスで、BACnet は BMS の側の IP アドレスで待ち受ける。

### 設定

設定は、実行ファイルと同じフォルダの setting.ini に書く。

| 項目 | 内容 | 既定値 |
|---|---|---|
| mqttip | MQTT ブローカーとして待ち受ける IP アドレス (PoEM Pod の側) | — |
| bacip | BACnet で待ち受ける IP アドレス (BMS の側) | — |
| bacport | BACnet/IP の UDP ポート | 47808 |
| bacdevice | BACnet の Device 番号。auto なら bacip の末尾の数 | auto |
| bacname | BACnet の Device の名前 | PoEM_Server_{Device 番号} |
| console | 受けた電文を画面に表示するか | true |
| debug | MQTT の詳しい記録を表示するか | false |

### 複数の PoEM Server

1 つの BACnet のネットワークに、複数の PoEM Server を置いてよい。その場合は、PoEM Server ごとに BMS の側の IP アドレスを分け、Device 番号が重ならないようにしなければならない。

- ポートは、すべての PoEM Server で 47808 とする。BACnet の通信相手は IP アドレスとポートの組で区別されるので、IP アドレスが違えば同じポートでよい。
- Device 番号を IP アドレスの末尾から決める (auto) 場合、同じ BACnet のネットワークに、末尾が同じ IP アドレスの PoEM Server を置いてはならない (BACnet のルータを越えた別のサブネットを含む)。その場合は bacdevice で番号を明示する。
- 1 台の計算機で複数の PoEM Server を動かすことは推奨しない。BACnet の同報 (Who-Is) の受信が不安定になることがある。

## BACnet Device

- PoEM Server は、1 台の BACnet Device として振る舞わなければならない。PoEM Pod の台数によらず、Device は 1 台とする。
- Device 番号、ポート、名前は、5.1.3 項の設定に従う。
- Device は、BACnet の Who-Is に I-Am で応答し、オブジェクトの読み出し (ReadProperty、ReadPropertyMultiple) と COV の購読に応じる。

## オブジェクトの番号の規則

### 番号の範囲

オブジェクトの番号 (Instance) は、PoEM Pod の番号と、子機の I²C アドレスと、項目から決める。

| 範囲 | 決め方 | 内容 |
|---|---|---|
| PoEM Pod の範囲 | pbase = PoEM Pod の番号 × 12000 | PoEM Pod 1 台に 12000 個の番号を使う |
| PoEM Pod の情報 | pbase + 0〜3 | 下表 |
| 子機の範囲 | base = pbase + I²C アドレス × 100 | 子機 1 台に 100 個の番号を使う。I²C アドレス 0x08〜0x77 (8〜119) に対し pbase + 800〜11999 |

- 番号の上限 (4194302) から、1 台の PoEM Server が扱える PoEM Pod は 349 台までとする。
- 1 台の Device の中では、オブジェクトの種類が違っても、同じ番号を使ってはならない (1.3 節の原則 3)。

### PoEM Pod の番号

- PoEM Server は、初めて電文を受け取った PoEM Pod に、空いている最も小さい番号 (0 から) を振り、PodMap.json (実行ファイルと同じフォルダ) に保存しなければならない。
- 再起動しても、同じ PoEM Pod には同じ番号を振らなければならない。
- 利用者は、PodMap.json を書き換えて PoEM Server を再起動することで、PoEM Pod の番号を変えてよい。

解説: PoEM Pod の番号は、PoEM Server ごとに独立している。別の PoEM Server につなぎ替えた PoEM Pod は、つなぎ替えた先で別の番号になることがある。PoEM Server ごとに Device が違うので、別の PoEM Server の間でオブジェクトの番号が重なっても問題はない。

### PoEM Pod の情報のオブジェクト

| 番号 | 種類 | 内容 |
|---|---|---|
| pbase + 0 | CharacterString Value | PoEM Pod の名前 (空なら PoEM Pod の ID) |
| pbase + 1 | CharacterString Value | PoEM Pod の ID (MAC アドレス、16 進 12 桁) |
| pbase + 2 | CharacterString Value | PoEM Pod のファームウェアの版 |
| pbase + 3 | CharacterString Value | PoEM Pod の IP アドレス |

### 子機のオブジェクト

| 番号 | 種類 | 内容 |
|---|---|---|
| base + 0 | DateTime Value | 最後に data を受け取った日時 |
| base + 1 | CharacterString Value | 子機の表示用 ID (5.5 節)。Description に UID を書く |
| base + 2 | CharacterString Value | 子機の名前 |
| base + 3 | Analog Input | 状態が正常でないチャネルのビット (bit i がチャネル i) |
| base + 4 | Analog Input | sample_seq |
| base + 5 | Analog Input | チャネル数 |
| base + 6 | DateTime Value | 最後にすべてのチャネルが正常だった日時 |
| base + 7 | CharacterString Value | 機種 (機種名と model_id) |
| base + 8 | CharacterString Value | 子機のファームウェアの版 |
| base + 9 | Analog Input | 子機の稼働時間 [s] |
| base + 10〜25 | Analog Input | チャネル 0〜15 の値。単位はチャネルの単位 |
| base + 26〜99 | — | 予約 |

解説: 項目ごとにオブジェクトの種類が決まっているので、子機の範囲の中で番号は重ならない。チャネルの値の番号は base + 10 + チャネル番号であり、同じ機種の子機に交換しても変わらない。

## オブジェクトの名前の規則

### 現在の形

Object_Name は、次の形で付ける。

P{PoEM Pod の番号 2 桁}\_A{I²C アドレス 10 進 3 桁}\_{子機の名前}\_{項目}

例: P01_A016_第2会議室 室内温度_Temp

| 部分 | 内容 |
|---|---|
| P01_A016 | PoEM Pod の番号と I²C アドレス |
| 子機の名前 | 子機に設定した名前。空なら「Node_」と表示用 ID |
| 項目 | チャネル名 (Temp など) または項目名 (DT、ID、NAME、FAULT、SEQ、CNT、NDT、MODEL、FW、UPTIME) |

PoEM Pod の情報のオブジェクトは、P{番号}_POD_NAME、P{番号}_POD_ID、P{番号}_POD_FW、P{番号}_POD_IP とする。

- 1 台の Device の中で、Object_Name は重なってはならない。P と A の部分で場所を区別するので、子機の名前が重なっても Object_Name は重ならない。
- 子機の名前は UTF-8 であり、BACnet の文字列 (UTF-8) としてそのまま入れる。

### 決まっていないこと

次の点は未決である (付録 F)。

- TSC コードを Object_Name に含めるかどうかと、その形
- 表示用 ID を Object_Name に含めるかどうか
- Object_Name の長さの上限と、日本語を扱えない BMS への対応
- 子機の名前を変えたときに Object_Name を変えるかどうか (現在は、名前を変えても Object_Name は変わらず、base + 2 の値だけが変わる)

## 表示用 ID

表示用 ID は、人が子機の個体を見分けるための短い値である。識別子 (キー) として使ってはならない。個体の識別には UID を使う。

- 表示用 ID は、UID (16 B) を FNV-1a (32 ビット) で計算し、上位と下位を XOR で 22 ビットに畳んだ値とする。値が 0x3FFFFF の場合は 0x3FFFFE とする。
- 表示は 10 進とする。

| 手順 | 計算 |
|---|---|
| 1 | h = 2166136261 |
| 2 | UID の各バイト x について、h = (h XOR x) × 16777619 (32 ビットで切り捨て) |
| 3 | h = (h XOR (h >> 22)) AND 0x3FFFFF |
| 4 | h が 0x3FFFFF なら 0x3FFFFE とする |

解説: 22 ビットでは、子機が多いと値が重なることがある (100 台で約 0.1% の確率)。表示用 ID は見分けるための補助であり、重なっても UID で区別できる。表示を 16 進 6 桁にする案があり、未決である (付録 F)。

## 状態の反映

- PoEM Server は、data の st の値を、チャネルのオブジェクトの Reliability に書かなければならない。
- st が 0 でない間は、STATUS_FLAGS の FAULT を立て、Present_Value を更新してはならない (最後の正常値のまま据え置く)。
- st が 0 に戻ったら、FAULT を下ろし、Present_Value の更新を再開する。
- data に値が無い (子機を読めなかった) 場合は、Present_Value を更新しない。
- base + 0 (最後に data を受け取った日時) は data を受け取るたびに、base + 6 (最後にすべてのチャネルが正常だった日時) はすべての st が 0 のときに更新する。

### 受信の途絶

PoEM Pod が停止したり LAN が切れたりすると、data が届かなくなる。現在の PoEM Server は、このとき値を最後のまま保ち、異常を示さない (base + 0 の日時が止まるだけである)。一定時間 data が届かない場合に Reliability を 12 (通信失敗) にする判定は、未決である (付録 F)。

## 構成の変化への追従

| 変化 | PoEM Server の動き |
|---|---|
| 子機を初めて受け取った (config) | 子機のオブジェクトを作る |
| 同じアドレスで UID が変わった (交換) | 子機のオブジェクトを作り直す (番号は変わらない) |
| 同じアドレスでチャネルの構成が変わった | 子機のオブジェクトを作り直す |
| 同じ UID が別のアドレスに現れた (アドレスの変更) | 古いアドレスのオブジェクトを消し、新しいアドレスに作る |
| 名前・ファームウェアの版が変わった | 該当するオブジェクトの値を更新する |
| config を受けていない子機の data | 無視する (config を待つ) |

- 撤去した子機のオブジェクトは、PoEM Server を再起動するまで残る。撤去を PoEM Server に伝える方法は未決である (付録 F)。

## TSC コードの管理

TSC コードは、建築設備分野で定義されている名前付けの規則による点の名前である。BMS の点の一覧と PoEM の点を対応させるために、PoEM Server の側で TSC コードを管理することを予定している。登録・編集の方法、保存先、場所 (PoEM Pod の番号と I²C アドレス) に付けるか個体 (UID) に付けるかは未決である (付録 F)。
