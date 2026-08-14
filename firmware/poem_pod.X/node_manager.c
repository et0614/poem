#include <stdio.h>
#include <string.h>
#include <math.h>
#include "node_manager.h"
#include "i2c_master.h"

slave_node_t NM_Nodes[MAX_NODES];
uint8_t      NM_NodeCount = 0;

// スキャンを実行した累計 (診断用)。
//
// スキャンは 0x08~0x77 を総当たりするので、バスが不調な最中に走ると 1 回で
// 100 回前後の失敗を積む。ポーリング由来の失敗 (1 周期あたり子機の数だけ) とは
// 桁が違うため、この値が同時に増えたかどうかを見ないと fail の急増を誤読する。
uint16_t     NM_ScanCount = 0;

void NM_init(void)
{
    NM_NodeCount = 0;
    memset(NM_Nodes, 0, sizeof(NM_Nodes));
}

// 指定した I²C アドレスの子機から共通領域の固定情報 (INFO BLOCK 0x00~0x27) を読み取り、
// node に格納する。検出と同時に呼ばれる前提で、status/value はゼロ初期化のまま、
// seq は SEQ_INITIAL に設定する。
static bool readInfoBlock(uint8_t i2c_addr, slave_node_t* node)
{
    uint8_t reg = INFO_BLOCK_OFFSET;
    uint8_t buf[INFO_BLOCK_LEN];

    if (!I2C_WriteRead(i2c_addr, &reg, 1, buf, INFO_BLOCK_LEN)) return false;

    node->i2c_addr = i2c_addr;

    // 0x00~0x03: Device ID (uint32 LE)
    node->device_id =  ((uint32_t)buf[0])
                    | (((uint32_t)buf[1]) << 8)
                    | (((uint32_t)buf[2]) << 16)
                    | (((uint32_t)buf[3]) << 24);

    // 0x06: Data Count
    uint8_t cnt = buf[REG_DATA_COUNT - INFO_BLOCK_OFFSET];
    if (cnt > NODE_MAX_VALUES) cnt = NODE_MAX_VALUES;
    node->data_count = cnt;

    // 0x08~0x17: Unit Type[8] (uint16 LE × 8)
    for (uint8_t i = 0; i < NODE_MAX_VALUES; i++) {
        uint8_t off = (REG_UNIT_TYPE - INFO_BLOCK_OFFSET) + i * 2;
        node->unit_type[i] = ((uint16_t)buf[off]) | (((uint16_t)buf[off + 1]) << 8);
    }

    // 0x18~0x27: Name (char[16], NUL 終端)
    memcpy(node->name, &buf[REG_NAME - INFO_BLOCK_OFFSET], NODE_NAME_LEN);
    node->name[NODE_NAME_LEN - 1] = '\0';
    // EEPROM 未初期化 (0xFF) を空文字扱いにする
    if ((unsigned char)node->name[0] == 0xFF) node->name[0] = '\0';

    // 計測関連は未取得状態で初期化
    node->status1 = 0;
    node->status2 = 0;
    node->seq     = SEQ_INITIAL;
    memset(node->value, 0, sizeof(node->value));

    return true;
}

bool NM_writeName(slave_node_t* node, const char* new_name)
{
    uint8_t payload[1 + NODE_NAME_LEN];
    payload[0] = REG_NAME;
    memset(&payload[1], 0, NODE_NAME_LEN);
    strncpy((char*)&payload[1], new_name, NODE_NAME_LEN - 1);

    if (!I2C_Write(node->i2c_addr, payload, sizeof(payload))) return false;

    // ローカル cache を同期
    memcpy(node->name, &payload[1], NODE_NAME_LEN);
    return true;
}

bool NM_writeAddress(slave_node_t* node, uint8_t new_addr)
{
    if (new_addr < I2C_ADDR_SCAN_FIRST || new_addr > I2C_ADDR_SCAN_LAST) return false;
    if (new_addr == node->i2c_addr) return true;  // 変更なしは成功扱い

    // [REG_ADDR_KEY] -> ADDR_KEY_UNLOCK (鍵)、続けて [REG_NEW_ADDR] -> new_addr。
    // 子機の reg_pointer は最初の 1byte (REG_ADDR_KEY=0x04) を起点に自動インクリメントする。
    // STOP 受信で子機が EEPROM 永続化 + SADDR 即時反映を行うので、これ以降は new_addr で応答する。
    uint8_t payload[3] = { REG_ADDR_KEY, ADDR_KEY_UNLOCK, new_addr };
    if (!I2C_Write(node->i2c_addr, payload, sizeof(payload))) return false;

    node->i2c_addr = new_addr;
    return true;
}

// I²C 多分岐で配線容量が大きいとき、まれに NACK 取りこぼしや読み出しビット化けが
// 起きる。スキャンは検出漏れが致命的 (子機が完全に「居ない」扱いになる) なので、
// アドレスチェックと情報読出のそれぞれを 3 回までリトライする。
#define SCAN_RETRY_MAX  3

// 試験用のダミー子機数。実子機が揃っていなくても Web UI (デバイス一覧のページング等) を
// 確認できるよう、スキャン後にリストへ架空の子機を追加する。実運用ビルドでは 0 にすること。
#define NM_DUMMY_NODE_COUNT 0

#if NM_DUMMY_NODE_COUNT > 0
// 実子機と衝突しない I²C アドレスを走査上限 (0x77) 側から割り当ててダミーを追加する。
// 実在しないため計測値は全て stale (status1=0xFF) とし、名前/アドレスの書き込みは
// I²C 応答がなく失敗するだけで実害はない。
static void appendDummyNodes(void)
{
    uint8_t addr = I2C_ADDR_SCAN_LAST;
    for (uint8_t i = 0; i < NM_DUMMY_NODE_COUNT && NM_NodeCount < MAX_NODES; i++) {
        // 実子機・追加済みダミーと重複しないアドレスまで下る
        while (addr >= I2C_ADDR_SCAN_FIRST) {
            bool used = false;
            for (uint8_t j = 0; j < NM_NodeCount; j++) {
                if (NM_Nodes[j].i2c_addr == addr) { used = true; break; }
            }
            if (!used) break;
            addr--;
        }
        if (addr < I2C_ADDR_SCAN_FIRST) return;  // 空きアドレスなし

        slave_node_t* nd = &NM_Nodes[NM_NodeCount];
        memset(nd, 0, sizeof(*nd));
        nd->i2c_addr   = addr--;
        nd->device_id  = 900001UL + i;
        nd->data_count = 2;
        nd->status1    = STATUS1_ALL_STALE;
        nd->seq        = SEQ_INITIAL;
        snprintf(nd->name, NODE_NAME_LEN, "DUMMY%02u", (unsigned)(i + 1));
        NM_NodeCount++;
    }
}
#endif

uint8_t NM_scan(void)
{
    if (NM_ScanCount < 0xFFFF) NM_ScanCount++;

    NM_NodeCount = 0;
    memset(NM_Nodes, 0, sizeof(NM_Nodes));

    for (uint8_t addr = I2C_ADDR_SCAN_FIRST;
         addr <= I2C_ADDR_SCAN_LAST && NM_NodeCount < MAX_NODES;
         addr++)
    {
        // アドレス応答チェック (リトライ付き)
        bool found = false;
        for (uint8_t attempt = 0; attempt < SCAN_RETRY_MAX; attempt++) {
            if (I2C_IsConnected(addr)) { found = true; break; }
        }
        if (!found) continue;

        // 固有情報の読み出し (リトライ付き)。失敗したアドレスは黙って捨てる。
        for (uint8_t attempt = 0; attempt < SCAN_RETRY_MAX; attempt++) {
            if (readInfoBlock(addr, &NM_Nodes[NM_NodeCount])) {
                NM_NodeCount++;
                break;
            }
        }
    }

#if NM_DUMMY_NODE_COUNT > 0
    appendDummyNodes();
#endif

    return NM_NodeCount;
}

// 稼働中の増設検出用の増分スキャン: 既存リストに無いアドレスだけを走査して追加する。
// NM_scan と違い既存ノードの状態 (seq 等) を保持する。戻り値は新規追加数。
// リトライは行わない (周期的に呼ばれるため、次周期が実質のリトライになる)。
// 注意: 応答が途絶えた既存ノードのアドレスに「別の新しい子機」を付けても、リスト上は
// 旧個体のままになる。個体の入れ替えを反映するには SCAN ボタンで全走査すること。
uint8_t NM_scanNewNodes(void)
{
    if (NM_ScanCount < 0xFFFF) NM_ScanCount++;

    uint8_t added = 0;

    for (uint8_t addr = I2C_ADDR_SCAN_FIRST;
         addr <= I2C_ADDR_SCAN_LAST && NM_NodeCount < MAX_NODES;
         addr++)
    {
        bool known = false;
        for (uint8_t j = 0; j < NM_NodeCount; j++) {
            if (NM_Nodes[j].i2c_addr == addr) { known = true; break; }
        }
        if (known) continue;

        if (!I2C_IsConnected(addr)) continue;

        if (readInfoBlock(addr, &NM_Nodes[NM_NodeCount])) {
            NM_NodeCount++;
            added++;
        }
    }

    return added;
}

bool NM_pollOne(slave_node_t* node)
{
    uint8_t reg = POLL_BLOCK_OFFSET;
    uint8_t buf[POLL_BLOCK_LEN];

    if (!I2C_WriteRead(node->i2c_addr, &reg, 1, buf, POLL_BLOCK_LEN)) return false;

    // 0x18: Status1
    node->status1 = buf[REG_STATUS1 - POLL_BLOCK_OFFSET];
    // 0x19: Status2
    node->status2 = buf[REG_STATUS2 - POLL_BLOCK_OFFSET];

    // 0x2A/0x2B: 子機の自己診断カウンタ。未対応の子機は 0 のまま。
    node->hold_count = buf[REG_HOLD_COUNT - POLL_BLOCK_OFFSET];
    node->err_count  = buf[REG_ERR_COUNT  - POLL_BLOCK_OFFSET];

    // 0x1C~0x3B: float[8] LE — Status2==1 (子機が新値を準備済み) のときだけ取り込む。
    // それ以外は前回値を保持し、新旧は seq (=sq) で表現する。
    // ※ slave 側は ATOMIC_BLOCK で書き込んでいても、こちらの read は 1 バイトずつ
    //   別 ISR に分かれるため、status2==0 のタイミングの float 領域はバイト裂け
    //   (tearing) で 0 などに化ける可能性がある。これを取り込まないことで防ぐ。
    // ※ status2 のバイト自体が化けて 1 に見えた場合、上のガードは抜けてしまう。
    //   その場合 float 領域は任意のビットパターンになり、指数部が全 1 なら NaN/Inf になる。
    //   NaN を value[] に入れると NM_buildDataJson が "nan" を出力し、JSON として不正なため
    //   PoEMServer 側のデシリアライズがメッセージ全体で失敗する。さらに NaN は次の正常値が
    //   来るまで value[] に残り続けるので、その子機は恒久的に更新が止まる。
    //   そこで 1 要素ずつ検査し、有限値でなければ取り込まず前回値を維持して stale を立てる。
    if (node->status2 == 1) {
        // AVR は LE なので memcpy で OK
        const uint8_t* src = &buf[REG_VALUE - POLL_BLOCK_OFFSET];
        for (uint8_t i = 0; i < NODE_MAX_VALUES; i++) {
            float v;
            memcpy(&v, src + (uint16_t)i * sizeof(float), sizeof(float));
            if (isfinite(v)) {
                node->value[i] = v;
            } else if (i < node->data_count) {
                // stale ビットは有効長の範囲だけ立てる。範囲外まで立てると status1 が
                // 0x00 にならず、PoEMServer 側の「最終正常データ受信日時」が更新されなくなる。
                node->status1 |= (uint8_t)(1u << i);
            }
        }
    }

    return true;
}

bool NM_clearStatus2(slave_node_t* node)
{
    uint8_t writeBuf[2] = { REG_STATUS2, 0 };
    return I2C_Write(node->i2c_addr, writeBuf, sizeof(writeBuf));
}

// MQTT トピック生成 ----------------------------------------------------------

void NM_buildConfigTopic(const slave_node_t* node, char* buf, uint16_t bufsize)
{
    snprintf(buf, bufsize, "nodes/config/%lu", (unsigned long)node->device_id);
}

void NM_buildDataTopic(const slave_node_t* node, char* buf, uint16_t bufsize)
{
    snprintf(buf, bufsize, "nodes/data/%lu", (unsigned long)node->device_id);
}

// JSON ビルダ -----------------------------------------------------------------

// 浮動小数点を有効数字 4 桁に丸める。
// NaN/Inf はそのまま返す (snprintf 側で "nan"/"inf" になる)。
static float roundTo4Sigfig(float x)
{
    if (!isfinite(x) || x == 0.0f) return x;
    float magnitude = floorf(log10f(fabsf(x)));
    float factor    = powf(10.0f, 3 - magnitude); // 4 桁目で丸める
    return roundf(x * factor) / factor;
}

uint16_t NM_buildConfigJson(const slave_node_t* node, char* buf, uint16_t bufsize)
{
    if (bufsize == 0) return 0;
    char* p   = buf;
    char* end = buf + bufsize;
    int   n;

    // adr (I²Cアドレス) は PoEMServer 側で BACnet インスタンス番号の基数 (adr×100) に使う
    n = snprintf(p, end - p, "{\"id\":%lu,\"adr\":%u,\"name\":\"%s\",\"cnt\":%u,\"u\":[",
                 (unsigned long)node->device_id, (unsigned)node->i2c_addr,
                 node->name, (unsigned)node->data_count);
    if (n < 0 || n >= end - p) return 0;
    p += n;

    for (uint8_t i = 0; i < node->data_count; i++) {
        n = snprintf(p, end - p, "%s%u",
                     (i == 0 ? "" : ","),
                     (unsigned)node->unit_type[i]);
        if (n < 0 || n >= end - p) return 0;
        p += n;
    }

    n = snprintf(p, end - p, "]}");
    if (n < 0 || n >= end - p) return 0;
    p += n;

    return (uint16_t)(p - buf);
}

uint16_t NM_buildDataJson(const slave_node_t* node, char* buf, uint16_t bufsize)
{
    if (bufsize == 0) return 0;
    char* p   = buf;
    char* end = buf + bufsize;
    int   n;

    n = snprintf(p, end - p, "{\"id\":%lu,\"st\":%u,\"sq\":%u,\"v\":[",
                 (unsigned long)node->device_id,
                 (unsigned)node->status1,
                 (unsigned)node->seq);
    if (n < 0 || n >= end - p) return 0;
    p += n;

    for (uint8_t i = 0; i < node->data_count; i++) {
        float v = roundTo4Sigfig(node->value[i]);
        // %g は有効数字 6 桁が既定。precision を 4 に絞り、不要なゼロは省略させる。
        n = snprintf(p, end - p, "%s%.4g",
                     (i == 0 ? "" : ","),
                     (double)v);
        if (n < 0 || n >= end - p) return 0;
        p += n;
    }

    n = snprintf(p, end - p, "]}");
    if (n < 0 || n >= end - p) return 0;
    p += n;

    return (uint16_t)(p - buf);
}
