#include <stdio.h>
#include <string.h>
#include <math.h>
#include "node_manager.h"
#include "i2c_master.h"
#include "utility.h"

slave_node_t NM_Nodes[MAX_NODES];
uint8_t      NM_NodeCount = 0;

// スキャンを実行した累計 (診断用)。
//
// スキャンは 0x08~0x77 を総当たりするので、バスが不調な最中に走ると 1 回で
// 100 回前後の失敗を積む。ポーリング由来の失敗 (1 周期あたり子機の数だけ) とは
// 桁が違うため、この値が同時に増えたかどうかを見ないと fail の急増を誤読する。
uint16_t     NM_ScanCount = 0;

// 応答はあったが OSL v2 子機ではなかったアドレス (bit n = アドレス n)。
// 正体の分からない機器を 10 秒ごとに読みに行かないよう、全走査までは覚えておく。
static uint8_t s_foreign[16];

// CRC 不一致・失敗のときに読み直す回数
#define READ_TRIES  3

// <editor-fold defaultstate="collapsed" desc="小物">

// CRC-16/CCITT-FALSE (多項式 0x1021、初期値 0xFFFF、反転なし)。子機と同じ。
static uint16_t crc16(const uint8_t* data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// 末尾 2 B (LE) が先頭からの CRC と一致するか
static bool crc_ok(const uint8_t* block, uint16_t len)
{
    uint16_t stored = (uint16_t)block[len - 2] | ((uint16_t)block[len - 1] << 8);
    return crc16(block, len - 2) == stored;
}

static inline uint16_t get_u16(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// 番地 reg から len バイト読む (16bit 番地は上位バイトから送る)
static bool read_reg(uint8_t addr, uint16_t reg, uint8_t* buf, uint16_t len)
{
    const uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return I2C_WriteRead(addr, a, 2, buf, len);
}

// CRC 込みの区画を読み、一致するまで読み直す
static bool read_checked(uint8_t addr, uint16_t reg, uint8_t* buf, uint16_t len)
{
    for (uint8_t t = 0; t < READ_TRIES; t++) {
        if (read_reg(addr, reg, buf, len) && crc_ok(buf, len)) return true;
    }
    return false;
}

static inline bool is_foreign(uint8_t addr) { return (s_foreign[addr >> 3] >> (addr & 7)) & 1; }
static inline void set_foreign(uint8_t addr) { s_foreign[addr >> 3] |= (uint8_t)(1 << (addr & 7)); }

// device_id (16 B) を FNV-1a で 22bit に畳み込む。v1 の子機が SIGROW.SERNUM から作って
// いた値と同じ計算なので、AVR の子機は v1 と同じ ID になる (PoEMServer の登録が続く)。
static uint32_t fold_id(const uint8_t* uid)
{
    uint32_t h = fnv1a_32(uid, 16);
    h = (h ^ (h >> 22)) & 0x3FFFFFUL;
    if (h == 0x3FFFFFUL) h = 0x3FFFFEUL;
    return h;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="走査">

void NM_init(void)
{
    NM_NodeCount = 0;
    memset(NM_Nodes, 0, sizeof(NM_Nodes));
    memset(s_foreign, 0, sizeof(s_foreign));
}

// 1 つのアドレスを調べ、OSL v2 子機なら node を埋めて true を返す。
//
// まず番地を送らずに 32 B 読む (子機は 0x0000 の識別区画から返す)。この段階では
// 書き込みを一切しないので、同じバスの他の I2C 機器を書き換えない (仕様 §7.1)。
// "OSL" と CRC が確かめられた相手にだけ、番地を指定した読み出しを行う。
// 応答はあったが OSL 子機でなかったアドレスは s_foreign に記録する。
static bool probe_node(uint8_t addr, slave_node_t* node)
{
    uint8_t id[IDENT_LEN];
    bool responded = false;
    bool valid     = false;

    for (uint8_t t = 0; t < READ_TRIES && !valid; t++) {
        if (!I2C_Read(addr, id, IDENT_LEN)) {
            if (!responded) return false;       // 最初からアドレスに応答なし (NACK)
            continue;
        }
        responded = true;
        valid = (memcmp(id, "OSL", 3) == 0 && crc_ok(id, IDENT_LEN));
    }
    if (!valid) {
        // 応答はあるのに一度も "OSL" と CRC が揃わなかった → OSL 子機ではない
        set_foreign(addr);
        return false;
    }

    uint8_t n = id[11];
    if (n == 0 || n > NODE_MAX_VALUES) { set_foreign(addr); return false; }

    // チャネルの型 (整数型を float に直すため)
    uint8_t ch[16 * NODE_MAX_VALUES + 2];
    if (!read_checked(addr, REG_CHDESC, ch, 16 * n + 2)) return false;

    // 拡張識別 (拡張ステータスを読めるか)。未実装なら CRC 込みですべて 0。
    uint8_t ext[EXT_IDENT_LEN];
    bool has_ext = false;
    if (read_reg(addr, REG_EXT_IDENT, ext, EXT_IDENT_LEN) && crc_ok(ext, EXT_IDENT_LEN)) {
        has_ext = (get_u16(&ext[0]) & CAP_EXT_STATUS) != 0;
    }

    memset(node, 0, sizeof(*node));
    node->i2c_addr       = addr;
    node->device_id      = fold_id(&id[14]);
    node->data_count     = n;
    node->has_ext_status = has_ext;
    for (uint8_t i = 0; i < n; i++) {
        node->type_bits |= (uint32_t)(ch[16 * i + 2] & 0x03) << (2 * i);
    }
    node->stale_mask = 0xFFFF;      // まだ値を読んでいない
    return true;
}

uint8_t NM_scan(void)
{
    if (NM_ScanCount < 0xFFFF) NM_ScanCount++;

    NM_NodeCount = 0;
    memset(NM_Nodes, 0, sizeof(NM_Nodes));
    memset(s_foreign, 0, sizeof(s_foreign));    // 全走査では記録も作り直す

    for (uint8_t addr = I2C_ADDR_SCAN_FIRST;
         addr <= I2C_ADDR_SCAN_LAST && NM_NodeCount < MAX_NODES;
         addr++)
    {
        if (probe_node(addr, &NM_Nodes[NM_NodeCount])) NM_NodeCount++;
    }
    return NM_NodeCount;
}

uint8_t NM_scanNewNodes(void)
{
    if (NM_ScanCount < 0xFFFF) NM_ScanCount++;

    uint8_t added = 0;
    for (uint8_t addr = I2C_ADDR_SCAN_FIRST;
         addr <= I2C_ADDR_SCAN_LAST && NM_NodeCount < MAX_NODES;
         addr++)
    {
        if (is_foreign(addr)) continue;
        bool known = false;
        for (uint8_t j = 0; j < NM_NodeCount; j++) {
            if (NM_Nodes[j].i2c_addr == addr) { known = true; break; }
        }
        if (known) continue;

        if (probe_node(addr, &NM_Nodes[NM_NodeCount])) {
            NM_NodeCount++;
            added++;
        }
    }
    return added;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="計測値・名前・アドレス・診断">

bool NM_pollOne(slave_node_t* node)
{
    uint8_t n = node->data_count;
    uint16_t len = (uint16_t)(2 + 5 * n + (n & 1) + 2);
    uint8_t buf[2 + 5 * NODE_MAX_VALUES + 1 + 2];

    // CRC を照合するので、v1 で起きていたバイト化けや NaN の混入はここで捨てられる。
    // POLL は番地を指定した瞬間に子機が一括で写し取るので、値が裂けることもない。
    if (!read_checked(node->i2c_addr, REG_POLL, buf, len)) {
        node->comm_ok = false;
        return false;
    }

    node->comm_ok    = true;
    node->sample_seq = get_u16(&buf[0]);
    uint16_t stale = 0;
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t* p = &buf[2 + 4 * i];
        uint8_t type = (uint8_t)((node->type_bits >> (2 * i)) & 0x03);
        float v;
        if (type == VTYPE_INT32) {
            int32_t x;  memcpy(&x, p, 4);  v = (float)x;
        } else if (type == VTYPE_UINT32) {
            uint32_t x; memcpy(&x, p, 4);  v = (float)x;
        } else {
            memcpy(&v, p, 4);           // AVR は LE なのでそのまま
        }
        // 状態が正常でないチャネルは値を更新せず、前回値のまま異常ビットを立てる
        if (buf[2 + 4 * n + i] != 0 || !isfinite(v)) {
            stale |= (uint16_t)(1u << i);
        } else {
            node->value[i] = v;
        }
    }
    node->stale_mask = stale;
    return true;
}

bool NM_readName(const slave_node_t* node, char* buf)
{
    uint8_t b[NAME_LEN + 2];
    buf[0] = '\0';
    if (!read_checked(node->i2c_addr, REG_NAME, b, sizeof(b))) return false;
    memcpy(buf, b, NAME_LEN);
    buf[NAME_LEN - 1] = '\0';
    return true;
}

void NM_truncateUtf8(char* s, uint16_t max_bytes)
{
    uint16_t len = (uint16_t)strlen(s);
    if (len <= max_bytes) return;
    // 続きのバイト (10xxxxxx) で切らないよう、文字の先頭まで戻る
    while (max_bytes > 0 && ((uint8_t)s[max_bytes] & 0xC0) == 0x80) max_bytes--;
    s[max_bytes] = '\0';
}

bool NM_writeName(const slave_node_t* node, const char* new_name)
{
    uint8_t payload[2 + NAME_LEN];
    payload[0] = (uint8_t)(REG_NAME >> 8);
    payload[1] = (uint8_t)REG_NAME;
    memset(&payload[2], 0, NAME_LEN);
    strncpy((char*)&payload[2], new_name, NAME_LEN - 1);
    NM_truncateUtf8((char*)&payload[2], NAME_LEN - 1);

    // 64 B を 1 回の書き込みで送る (途中で STOP を挟むと名前が一旦切れて反映される)
    return I2C_Write(node->i2c_addr, payload, sizeof(payload));
}

bool NM_writeAddress(slave_node_t* node, uint8_t new_addr)
{
    if (new_addr < I2C_ADDR_SCAN_FIRST || new_addr > I2C_ADDR_SCAN_LAST) return false;
    if (new_addr == node->i2c_addr) return true;  // 変更なしは成功扱い

    // 鍵と新アドレスを 1 回の書き込みで送る。子機は STOP で EEPROM に保存し、
    // 即座に新アドレスへ切り替える (リセット不要)。
    const uint8_t payload[4] = {
        (uint8_t)(REG_ADDR_KEY >> 8), (uint8_t)REG_ADDR_KEY, ADDR_KEY_UNLOCK, new_addr
    };
    if (!I2C_Write(node->i2c_addr, payload, sizeof(payload))) return false;

    node->i2c_addr = new_addr;
    return true;
}

bool NM_readExtStatus(const slave_node_t* node, uint32_t* uptime_s, uint8_t* reset_cause,
                      uint16_t* err_i2c, uint16_t* err_sensor)
{
    if (!node->has_ext_status) return false;
    uint8_t b[EXT_STATUS_LEN];
    if (!read_checked(node->i2c_addr, REG_EXT_STATUS, b, sizeof(b))) return false;
    *uptime_s    = (uint32_t)get_u16(&b[0]) | ((uint32_t)get_u16(&b[2]) << 16);
    *reset_cause = b[5];
    *err_i2c     = get_u16(&b[6]);
    *err_sensor  = get_u16(&b[8]);
    return true;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="MQTT">

void NM_buildConfigTopic(const slave_node_t* node, char* buf, uint16_t bufsize)
{
    snprintf(buf, bufsize, "nodes/config/%lu", (unsigned long)node->device_id);
}

void NM_buildDataTopic(const slave_node_t* node, char* buf, uint16_t bufsize)
{
    snprintf(buf, bufsize, "nodes/data/%lu", (unsigned long)node->device_id);
}

// 浮動小数点を有効数字 6 桁に丸める。NaN/Inf はそのまま返す。
//
// 4 桁だと絶対圧センサ (BMP585) の大気圧 101325 Pa が 101300 Pa になり、分解能が
// 100 Pa まで落ちる。6 桁なら 1e5 付近で 1 Pa 刻み。float の仮数は 24 bit (約 7 桁)
// なので、x * factor が 999999.5 以下に収まるこの丸めは誤差なく動く。
static float roundTo6Sigfig(float x)
{
    if (!isfinite(x) || x == 0.0f) return x;
    float magnitude = floorf(log10f(fabsf(x)));
    float factor    = powf(10.0f, 5 - magnitude);
    return roundf(x * factor) / factor;
}

// JSON の文字列として書き出す (" と \ と制御文字をエスケープ。UTF-8 はそのまま)
static int json_str(char* p, char* end, const char* s)
{
    char* start = p;
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        if (end - p < 7) return -1;
        if (c == '"' || c == '\\') { *p++ = '\\'; *p++ = (char)c; }
        else if (c < 0x20)         { p += snprintf(p, end - p, "\\u%04x", c); }
        else                       { *p++ = (char)c; }
    }
    return (int)(p - start);
}

uint16_t NM_buildConfigJson(const slave_node_t* node, char* buf, uint16_t bufsize)
{
    if (bufsize == 0) return 0;
    char* p   = buf;
    char* end = buf + bufsize;
    int   n;

    // 名前とチャネル記述は保持していないので、ここで子機から読む
    char name[NAME_LEN];
    NM_readName(node, name);                        // 読めなければ空文字
    uint8_t cnt = node->data_count;
    uint8_t ch[16 * NODE_MAX_VALUES + 2];
    if (!read_checked(node->i2c_addr, REG_CHDESC, ch, 16 * cnt + 2)) return 0;
    if (cnt > MQTT_MAX_VALUES) cnt = MQTT_MAX_VALUES;

    // adr (I²Cアドレス) は PoEMServer 側で BACnet インスタンス番号の基数 (adr×100) に使う
    n = snprintf(p, end - p, "{\"id\":%lu,\"adr\":%u,\"name\":\"",
                 (unsigned long)node->device_id, (unsigned)node->i2c_addr);
    if (n < 0 || n >= end - p) return 0;
    p += n;
    n = json_str(p, end, name);
    if (n < 0) return 0;
    p += n;
    n = snprintf(p, end - p, "\",\"cnt\":%u,\"u\":[", (unsigned)cnt);
    if (n < 0 || n >= end - p) return 0;
    p += n;

    for (uint8_t i = 0; i < cnt; i++) {
        n = snprintf(p, end - p, "%s%u", (i == 0 ? "" : ","), (unsigned)get_u16(&ch[16 * i]));
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

    uint8_t cnt = node->data_count;
    if (cnt > MQTT_MAX_VALUES) cnt = MQTT_MAX_VALUES;

    // st: 読めなかったときは 0xFF、それ以外はチャネルごとの異常ビット (8 チャネルぶん)
    // sq: sample_seq の下位 8bit。進み方が崩れれば、その子機が止まった・再起動したと分かる。
    unsigned st = node->comm_ok ? (unsigned)(node->stale_mask & 0xFF) : STATUS1_ALL_STALE;
    n = snprintf(p, end - p, "{\"id\":%lu,\"st\":%u,\"sq\":%u,\"v\":[",
                 (unsigned long)node->device_id, st, (unsigned)(node->sample_seq & 0xFF));
    if (n < 0 || n >= end - p) return 0;
    p += n;

    for (uint8_t i = 0; i < cnt; i++) {
        // 値は CRC を通ったものだけを取り込んでいるが、念のため有限でなければ 0 を送る
        // (NaN を出すと JSON として不正になり、PoEMServer がメッセージ全体を捨てる)
        float v = isfinite(node->value[i]) ? roundTo6Sigfig(node->value[i]) : 0.0f;
        n = snprintf(p, end - p, "%s%.6g", (i == 0 ? "" : ","), (double)v);
        if (n < 0 || n >= end - p) return 0;
        p += n;
    }

    n = snprintf(p, end - p, "]}");
    if (n < 0 || n >= end - p) return 0;
    p += n;
    return (uint16_t)(p - buf);
}

// </editor-fold>
