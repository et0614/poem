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

    // チャネルの型 (値を型どおりに JSON にするため)
    uint8_t ch[16 * NODE_MAX_VALUES + 2];
    if (!read_checked(addr, REG_CHDESC, ch, 16 * n + 2)) return false;

    // 拡張識別 (拡張ステータスを読めるか、有効桁数があるか)。未実装なら CRC 込みですべて 0。
    uint8_t ext[EXT_IDENT_LEN];
    uint16_t cap = 0;
    if (read_reg(addr, REG_EXT_IDENT, ext, EXT_IDENT_LEN) && crc_ok(ext, EXT_IDENT_LEN)) {
        cap = get_u16(&ext[0]);
    }

    memset(node, 0, sizeof(*node));
    node->i2c_addr       = addr;
    node->data_count     = n;
    node->has_ext_status = (cap & CAP_EXT_STATUS) != 0;
    for (uint8_t i = 0; i < n; i++) {
        node->type_bits |= (uint32_t)(ch[16 * i + 2] & 0x03) << (2 * i);
    }

    // 有効桁数 (拡張チャネル属性)。無ければ 0 = 既定の 6 桁
    if (cap & CAP_CH_ATTR) {
        uint8_t attr[4 * NODE_MAX_VALUES + 2];
        if (read_checked(addr, REG_EXT_ATTR, attr, 4 * n + 2)) {
            for (uint8_t i = 0; i < n; i++) {
                uint8_t prec = attr[4 * i];
                if (prec > 15) prec = 15;
                node->prec_bits |= (uint64_t)prec << (4 * i);
            }
        }
    }
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

bool NM_readIdent(const slave_node_t* node, uint8_t* buf)
{
    return read_checked(node->i2c_addr, REG_IDENT, buf, IDENT_LEN)
        && memcmp(buf, "OSL", 3) == 0;
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
    if (len > max_bytes) {
        // 続きのバイト (10xxxxxx) で切らないよう、文字の先頭まで戻る
        while (max_bytes > 0 && ((uint8_t)s[max_bytes] & 0xC0) == 0x80) max_bytes--;
        s[max_bytes] = '\0';
        len = max_bytes;
    }
    // 末尾の多バイト文字が途中で切れていれば (先頭バイトが示す長さに足りなければ) 取り除く
    uint16_t i = len;
    while (i > 0 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) i--;
    if (i > 0) {
        uint8_t lead = (uint8_t)s[i - 1];
        uint8_t need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
        if ((uint16_t)(len - (i - 1)) < need) s[i - 1] = '\0';
    }
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

// <editor-fold defaultstate="collapsed" desc="MQTT v2">

void NM_buildTopic(const slave_node_t* node, const char* pod_id, const char* kind,
                   char* buf, uint16_t bufsize)
{
    snprintf(buf, bufsize, "poem/%s/node/%u/%s", pod_id, (unsigned)node->i2c_addr, kind);
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

// snprintf の結果を確かめて p を進める。収まらなければ 0 を返して抜ける。
#define EMIT(...) do { int _n = snprintf(p, end - p, __VA_ARGS__); \
                       if (_n < 0 || _n >= end - p) return 0; p += _n; } while (0)

uint16_t NM_buildConfigJson(const slave_node_t* node, char* buf, uint16_t bufsize)
{
    if (bufsize == 0) return 0;
    char* p   = buf;
    char* end = buf + bufsize;

    // 識別・名前・チャネル記述は保持していないので、ここで子機から読む
    uint8_t id[IDENT_LEN];
    if (!NM_readIdent(node, id)) return 0;
    char name[NAME_LEN];
    NM_readName(node, name);                        // 読めなければ空文字
    uint8_t n = node->data_count;
    uint8_t ch[16 * NODE_MAX_VALUES + 2];
    if (!read_checked(node->i2c_addr, REG_CHDESC, ch, 16 * n + 2)) return 0;

    // 拡張チャネル属性の flags (積算値・補助値)。無ければ 0
    uint8_t attr[4 * NODE_MAX_VALUES + 2];
    bool has_attr = node->prec_bits != 0
                 && read_checked(node->i2c_addr, REG_EXT_ATTR, attr, 4 * n + 2);

    // uid: 16 B の device_id を 16 進 32 文字で
    EMIT("{\"uid\":\"");
    for (uint8_t i = 0; i < 16; i++) EMIT("%02X", id[14 + i]);
    EMIT("\",\"vendor\":%u,\"model\":%u,\"fw\":\"%u.%u.%u\",\"period\":%u,\"name\":\"",
         (unsigned)get_u16(&id[4]), (unsigned)get_u16(&id[6]),
         (unsigned)id[8], (unsigned)id[9], (unsigned)id[10], (unsigned)get_u16(&id[12]));
    int w = json_str(p, end, name);
    if (w < 0) return 0;
    p += w;
    EMIT("\",\"ch\":[");

    for (uint8_t i = 0; i < n; i++) {
        const uint8_t* c = &ch[16 * i];
        char cname[13];
        memcpy(cname, &c[4], 12);
        cname[12] = '\0';
        EMIT("%s{\"n\":\"", (i == 0 ? "" : ","));
        w = json_str(p, end, cname);
        if (w < 0) return 0;
        p += w;
        EMIT("\",\"u\":%u,\"t\":%u,\"p\":%u,\"f\":%u}",
             (unsigned)get_u16(&c[0]), (unsigned)c[2],
             (unsigned)((node->prec_bits >> (4 * i)) & 0x0F),
             has_attr ? (unsigned)attr[4 * i + 1] : 0u);
    }
    EMIT("]}");
    return (uint16_t)(p - buf);
}

// 値 1 つを型どおりに書く。float が有限でなければ 0 を書いて false を返す
// (呼び出し側でそのチャネルの状態を異常にする)。
static bool emit_value(char** pp, char* end, const uint8_t* raw, uint8_t type, uint8_t prec)
{
    char* p = *pp;
    int   n;
    bool  ok = true;
    if (type == VTYPE_INT32) {
        int32_t x;  memcpy(&x, raw, 4);
        n = snprintf(p, end - p, "%ld", (long)x);
    } else if (type == VTYPE_UINT32) {
        uint32_t x; memcpy(&x, raw, 4);
        n = snprintf(p, end - p, "%lu", (unsigned long)x);
    } else {
        float x;    memcpy(&x, raw, 4);         // AVR は LE なのでそのまま
        // NaN / Inf を出すと JSON として不正になり、サーバがメッセージ全体を捨てる
        if (!isfinite(x)) { x = 0.0f; ok = false; }
        n = snprintf(p, end - p, "%.*g", (int)(prec ? prec : 6), (double)x);
    }
    if (n > 0 && n < end - p) *pp = p + n;
    return ok;
}

bool NM_pollAndBuildDataJson(slave_node_t* node, char* buf, uint16_t bufsize, uint16_t* len)
{
    *len = 0;
    if (bufsize < 16) return false;
    char* p   = buf;
    char* end = buf + bufsize;
    uint8_t n = node->data_count;
    int k;

    uint16_t plen = (uint16_t)(2 + 5 * n + (n & 1) + 2);
    uint8_t  poll[2 + 5 * NODE_MAX_VALUES + 1 + 2];

    // CRC を照合するので、化けたデータはここで捨てられる (v1 の NaN 混入の対策が要らない)。
    // POLL は番地を指定した瞬間に子機が一括で写し取るので、値が裂けることもない。
    node->comm_ok = read_checked(node->i2c_addr, REG_POLL, poll, plen);

    if (!node->comm_ok) {
        // 読めなかった: 全チャネルを通信失敗とし、値は送らない (サーバは前回値を保持する)
        k = snprintf(p, end - p, "{\"st\":[");
        if (k < 0 || k >= end - p) return false;
        p += k;
        for (uint8_t i = 0; i < n; i++) {
            k = snprintf(p, end - p, "%s%u", (i == 0 ? "" : ","), ST_COMMUNICATION_FAILURE);
            if (k < 0 || k >= end - p) return false;
            p += k;
        }
        k = snprintf(p, end - p, "],\"v\":[]}");
        if (k < 0 || k >= end - p) return false;
        p += k;
        *len = (uint16_t)(p - buf);
        return false;
    }

    // 値を先に書き、有限でない float があればそのチャネルの状態を異常にする
    uint8_t st[NODE_MAX_VALUES];
    memcpy(st, &poll[2 + 4 * n], n);

    char  vals[NODE_MAX_VALUES * 14 + 4];
    char* q    = vals;
    char* qend = vals + sizeof(vals) - 1;
    for (uint8_t i = 0; i < n; i++) {
        if (i) { if (qend - q < 2) return false; *q++ = ','; }
        uint8_t type = (uint8_t)((node->type_bits >> (2 * i)) & 0x03);
        uint8_t prec = (uint8_t)((node->prec_bits >> (4 * i)) & 0x0F);
        if (!emit_value(&q, qend, &poll[2 + 4 * i], type, prec) && st[i] == 0) {
            st[i] = ST_UNRELIABLE_OTHER;
        }
    }
    *q = '\0';

    k = snprintf(p, end - p, "{\"sq\":%u,\"st\":[", (unsigned)get_u16(&poll[0]));
    if (k < 0 || k >= end - p) return false;
    p += k;
    for (uint8_t i = 0; i < n; i++) {
        k = snprintf(p, end - p, "%s%u", (i == 0 ? "" : ","), (unsigned)st[i]);
        if (k < 0 || k >= end - p) return false;
        p += k;
    }
    k = snprintf(p, end - p, "],\"v\":[%s]}", vals);
    if (k < 0 || k >= end - p) return false;
    p += k;
    *len = (uint16_t)(p - buf);
    return true;
}

// </editor-fold>
