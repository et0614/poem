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

### 番号の振り方

オブジェクトは、BACnet の規格どおり、オブジェクトの種類 (Analog Input、CharacterString Value、DateTime Value) と番号 (Instance) の組で区別する。

- PoEM Server は、オブジェクトの番号を、種類ごとに 0 から、欠番を設けずに詰めて振らなければならない。種類が違えば、同じ番号を使ってよい。
- 番号は、オブジェクトを初めて作るときに、その種類で使われている最も大きい番号の次の番号とする。
- PoEM Server は、振った番号を対応表 (ObjectMap.json、実行ファイルと同じフォルダ) に保存し、再起動しても同じ番号を使わなければならない。一度振った番号を変えてはならない。
- 対応表の鍵は、場所と項目 (PoEM Pod の番号、子機の I²C アドレス、項目) とする。同じ場所の子機を交換しても、番号は変わらない。
- 対応表にある番号は、そのオブジェクトが無くなっても、ほかのオブジェクトに使ってはならない。

解説: インスタンス番号の振り方は、BACnet Device の側 (機器を納める側) が決める事項であり、建物の場所の体系に合わせる必要はない。一般の現場では、オブジェクトの種類ごとに 0 から詰めて番号を振ることが多い。欠番の多い振り方は、BMS の点の一覧でかえって見にくい。一方で、番号を詰め直すと BMS の登録が崩れるので、一度振った番号は対応表で固定する。撤去した子機の番号は欠番として残る。

### 振る順序

番号は、オブジェクトを作る順に振る。PoEM Server は、次の順にオブジェクトを作らなければならない。

1. PoEM Pod を初めて受け取ったとき: その PoEM Pod の情報のオブジェクト (5.3.4 項)
2. 子機を初めて受け取ったとき (config): その子機のオブジェクト (5.3.5 項)。種類ごとに、5.3.5 項の表の順に振る

PoEM Pod は、全体の走査で子機をアドレスの順に見つけて config を送るので、初めて立ち上げたときの番号は、PoEM Pod の番号の順、子機のアドレスの順に並ぶ。

- 交換した子機のチャネル数が、前の子機より多い場合は、増えたチャネルにだけ新しい番号を振る (その種類の最後に足す)。少ない場合は、使わなくなった番号を欠番として残す。

### PoEM Pod の番号

- PoEM Server は、初めて電文を受け取った PoEM Pod に、空いている最も小さい番号 (0 から) を振り、PodMap.json (実行ファイルと同じフォルダ) に保存しなければならない。
- 再起動しても、同じ PoEM Pod には同じ番号を振らなければならない。
- 利用者は、PodMap.json を書き換えて PoEM Server を再起動することで、PoEM Pod の番号を変えてよい。

解説: PoEM Pod の番号は、オブジェクトの名前 (5.4 節) と対応表の鍵に使う。PoEM Pod を交換したときは、PodMap.json で新しい PoEM Pod に古い番号を割り当て直すと、オブジェクトの番号と名前を引き継げる。PoEM Pod の番号は PoEM Server ごとに独立している。

### PoEM Pod の情報のオブジェクト

| 順 | 種類 | 項目 | 内容 |
|---|---|---|---|
| 1 | CharacterString Value | POD_NAME | PoEM Pod の名前 (空なら PoEM Pod の ID) |
| 2 | CharacterString Value | POD_ID | PoEM Pod の ID (MAC アドレス、16 進 12 桁) |
| 3 | CharacterString Value | POD_FW | PoEM Pod のファームウェアの版 |
| 4 | CharacterString Value | POD_IP | PoEM Pod の IP アドレス |

### 子機のオブジェクト

| 順 | 種類 | 項目 | 内容 |
|---|---|---|---|
| 1 | Analog Input | CH0〜CH(N−1) | チャネル 0〜N−1 の値。単位はチャネルの単位 |
| 2 | Analog Input | FAULT | 状態が正常でないチャネルのビット (bit i がチャネル i) |
| 3 | Analog Input | SEQ | sample_seq |
| 4 | Analog Input | CNT | チャネル数 |
| 5 | Analog Input | UPTIME | 子機の稼働時間 [s] |
| 1 | CharacterString Value | ID | 子機の表示用 ID (5.5 節)。Description に UID を書く |
| 2 | CharacterString Value | NAME | 子機の名前 |
| 3 | CharacterString Value | MODEL | 機種 (機種名と model_id) |
| 4 | CharacterString Value | FW | 子機のファームウェアの版 |
| 1 | DateTime Value | DT | 最後に data を受け取った日時 |
| 2 | DateTime Value | NDT | 最後にすべてのチャネルが正常だった日時 |

解説: チャネルの値を各子機の Analog Input の先頭に置くので、BMS に登録する主な点 (チャネルの値) が、子機ごとに続いた番号になる。

## オブジェクトの名前の規則

### 現在の形

Object_Name は、次の形で付ける。

P{PoEM Pod の番号 2 桁}\_A{I²C アドレス 10 進 3 桁}\_{子機の名前}\_{項目}

例: P01_A016_第2会議室 室内温度_Temp

| 部分 | 内容 |
|---|---|
| P01_A016 | PoEM Pod の番号と I²C アドレス |
| 子機の名前 | 子機に設定した名前。空なら「Node_」と表示用 ID |
| 項目 | チャネル名 (Temp など) または項目名 (DT、ID、NAME、FAULT、SEQ、CNT、NDT、MODEL、FW、UPTIME。5.3.5 項) |

PoEM Pod の情報のオブジェクトは、P{番号}_POD_NAME、P{番号}_POD_ID、P{番号}_POD_FW、P{番号}_POD_IP とする。

- 1 台の Device の中で、Object_Name は重なってはならない。P と A の部分で場所を区別するので、子機の名前が重なっても Object_Name は重ならない。
- 子機の名前は UTF-8 であり、BACnet の文字列 (UTF-8) としてそのまま入れる。
- TSC コードは Object_Name に含めない。TSC コードと点の対応は、PoEM Server の側で管理する (5.8 節)。

### 決まっていないこと

次の点は未決である (付録 F)。

- 表示用 ID を Object_Name に含めるかどうか (現在は、子機の名前が空のときだけ含める)
- Object_Name の長さの上限と、日本語を扱えない BMS への対応
- 子機の名前を変えたときに Object_Name を変えるかどうか (現在は、名前を変えても Object_Name は変わらず、NAME の値だけが変わる)

## 表示用 ID

表示用 ID は、人が子機の個体を見分けるための短い値である。識別子 (キー) として使ってはならない。個体の識別には UID を使う。

- 表示用 ID は、UID (16 B) を FNV-1a (32 ビット) で計算し、上位と下位を XOR で 22 ビットに畳んだ値とする。値が 0x3FFFFF の場合は 0x3FFFFE とする。
- 表示は、22 ビットの値に 0x100000 を足し、16 進 6 桁 (英字は大文字) で表す。範囲は 100000〜4FFFFE である。

| 手順 | 計算 |
|---|---|
| 1 | h = 2166136261 |
| 2 | UID の各バイト x について、h = (h XOR x) × 16777619 (32 ビットで切り捨て) |
| 3 | h = (h XOR (h >> 22)) AND 0x3FFFFF |
| 4 | h が 0x3FFFFF なら 0x3FFFFE とする |
| 5 | h + 0x100000 を 16 進 6 桁で表す (例: h = 0x03A9F0 なら 13A9F0) |

解説: 22 ビットでは、子機が多いと値が重なることがある (100 台で約 0.1% の確率)。表示用 ID は見分けるための補助であり、重なっても UID で区別できる。人が覚えやすいよう、10 進 (最大 7 桁) より短い 16 進 6 桁とした。0x100000 を足すのは、桁数を常に 6 桁にそろえ、先頭が 0 にならないようにするためである (表計算ソフトなどで先頭の 0 が落ちない)。ただし、数字と E だけからなる値 (例: 12E345) は、表計算ソフトが指数表記の数値と見なすことがあるので、文字列として扱う。

## 状態の反映

- PoEM Server は、data の st の値を、チャネルのオブジェクトの Reliability に書かなければならない。
- st が 0 でない間は、STATUS_FLAGS の FAULT を立て、Present_Value を更新してはならない (最後の正常値のまま据え置く)。
- st が 0 に戻ったら、FAULT を下ろし、Present_Value の更新を再開する。
- data に値が無い (子機を読めなかった) 場合は、Present_Value を更新しない。
- DT (最後に data を受け取った日時) は data を受け取るたびに、NDT (最後にすべてのチャネルが正常だった日時) はすべての st が 0 のときに更新する。

### 受信の途絶

- PoEM Server は、子機の data を最後に受け取ってから 1 時間が過ぎたら、その子機のすべてのチャネルの Reliability を 12 (通信失敗) にし、STATUS_FLAGS の FAULT を立てなければならない。Present_Value は最後の値のまま据え置く。
- 再び data を受け取ったら、data の st に従って元に戻す。

解説: PoEM Pod が停止したり LAN が切れたりすると、data が届かなくなる。この判定が無いと、値が最後のまま正常に見え続ける。1 時間は、PoEM Pod の再起動や一時的なネットワークの切断で誤って異常にしない長さとして定めた。

## 構成の変化への追従

| 変化 | PoEM Server の動き |
|---|---|
| 子機を初めて受け取った (config) | 子機のオブジェクトを作る |
| 同じアドレスで UID が変わった (交換) | 子機のオブジェクトを作り直す (番号は対応表のまま変わらない) |
| 同じアドレスでチャネルの構成が変わった | 子機のオブジェクトを作り直す |
| 同じ UID が別のアドレスに現れた (アドレスの変更) | 古いアドレスのオブジェクトを消し、新しいアドレスのオブジェクトを作る (新しいアドレスの番号を振る。古いアドレスの番号は対応表に残る) |
| 名前・ファームウェアの版が変わった | 該当するオブジェクトの値を更新する |
| config を受けていない子機の data | 無視する (config を待つ) |

- PoEM Server は、撤去した子機のオブジェクトを自動で消してはならない。オブジェクトは PoEM Server を再起動するまで残す。

解説: 撤去した子機のオブジェクトは、受信の途絶 (5.6.1 項) により異常として残る。異常が起きて取り外された子機があった場合に、その存在が BMS から分かる。一定時間で自動で消すと、それが分からなくなる。オブジェクトの数は多くないので、残しても問題はない。

## TSC コードの管理

TSC コードは、建築設備分野で定義されている名前付けの規則による点の名前である。BMS の点の一覧と PoEM の点を対応させるために、PoEM Server の側で TSC コードを管理することを予定している。TSC コードは Object_Name に含めない (5.4 節)。登録・編集の方法、保存先、場所 (PoEM Pod の番号と I²C アドレス) に付けるか個体 (UID) に付けるかは未決である (付録 F)。
