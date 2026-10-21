#include "bridge.h"
#include "sysex.h"
#include "i2c_host.h"
#include "led.h"
#include "link_config.h"
#include "systime.h"
#include "tinyusb/tusb.h"
#include "mcc_generated_files/system/ccp.h"

#include <avr/io.h>
#include <avr/wdt.h>
#include <string.h>

// <editor-fold defaultstate="collapsed" desc="定義">

// コマンド (仕様 §4.1)
#define CMD_GET_INFO      0x01
#define CMD_SET_CONFIG    0x02
#define CMD_PROBE         0x10
#define CMD_SCAN          0x11
#define CMD_READ          0x20
#define CMD_WRITE         0x21
#define CMD_WRITE_READ    0x22
#define CMD_BATCH         0x30
#define CMD_BUS_STATUS    0x31
#define CMD_BUS_RECOVER   0x32
#define CMD_LED           0x38
#define CMD_REBOOT        0x3E

// 要求の形式に関する結果コード (仕様 §5)。I2C 系 (0x00–0x07) は i2c_result_t と同じ値。
#define RES_OK            0x00
#define RES_BAD_LENGTH    0x10
#define RES_BAD_PARAM     0x11
#define RES_UNKNOWN_CMD   0x12
#define RES_BAD_ENCODING  0x13

// GET_INFO の機能ビット
#define CAP_BATCH         0x0001
#define CAP_LED_IDENTIFY  0x0002
#define CAP_BUS_RECOVER   0x0004

// BATCH のフラグ
#define BATCH_STOP_ON_ERROR  0x01

// 応答の組み立て用。最も長いのは BATCH で、
// 結果 1 + 実行数 1 + 操作ごとの (結果 1 + 長さ 2) × 64 + データ 512 = 706 B。
#define RESP_MAX  (2 + 3 * MAX_BATCH_OPS + MAX_BATCH)
static uint8_t  s_resp[RESP_MAX];

// REBOOT は応答を送り終えてから行う
static bool s_reboot_requested = false;

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="小物">

static inline uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void reply(uint8_t seq, uint8_t cmd, uint16_t len)
{
    SYSEX_SendResponse(seq, cmd, s_resp, len);
}

static void reply_status(uint8_t seq, uint8_t cmd, uint8_t status)
{
    s_resp[0] = status;
    reply(seq, cmd, 1);
}

// I2C を 1 回使うたびに LED へ知らせる
static i2c_result_t done(i2c_result_t r)
{
    LED_Activity();
    return r;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="MIDI 標準の機器問い合わせ (Device Inquiry)">

// 問い合わせ: F0 7E <dev> 06 01 F7。応答は仕様 §3.4 の形。
static bool handle_device_inquiry(const uint8_t *msg, uint16_t len)
{
    if (len < 4 || msg[0] != 0x7E || msg[2] != 0x06 || msg[3] != 0x01) return false;

    uint8_t r[17];
    uint8_t n = 0;
    r[n++] = 0xF0;
    r[n++] = 0x7E;
    r[n++] = 0x7F;                     // 全機器宛て
    r[n++] = 0x06;
    r[n++] = 0x02;
    r[n++] = SYSEX_MFR_0;
#if SYSEX_MFR_LEN == 3
    r[n++] = SYSEX_MFR_1;
    r[n++] = SYSEX_MFR_2;
#endif
    r[n++] = DI_FAMILY & 0x7F;         // family / model は 7bit ずつ下位から
    r[n++] = (DI_FAMILY >> 7) & 0x7F;
    r[n++] = DI_MODEL & 0x7F;
    r[n++] = (DI_MODEL >> 7) & 0x7F;
    r[n++] = FW_VERSION_MAJOR & 0x7F;
    r[n++] = FW_VERSION_MINOR & 0x7F;
    r[n++] = FW_VERSION_PATCH & 0x7F;
    r[n++] = 0x00;
    r[n++] = 0xF7;
    SYSEX_SendRaw(r, n);
    return true;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="各コマンド">

static uint16_t cmd_get_info(const uint8_t *p, uint16_t plen)
{
    (void)p; (void)plen;
    uint8_t *r = s_resp;
    r[0] = RES_OK;
    r[1] = PROTOCOL_VERSION;
    r[2] = FW_VERSION_MAJOR;
    r[3] = FW_VERSION_MINOR;
    r[4] = FW_VERSION_PATCH;
    put_u16(&r[5],  MAX_XFER);
    put_u16(&r[7],  MAX_BATCH);
    put_u16(&r[9],  I2CH_GetSpeed());
    put_u16(&r[11], I2CH_GetTimeout());
    put_u16(&r[13], CAP_BATCH | CAP_LED_IDENTIFY | CAP_BUS_RECOVER);
    memcpy(&r[15], (const void *)&SIGROW.SERNUM0, 16);
    return 31;
}

static uint16_t cmd_set_config(const uint8_t *p, uint16_t plen)
{
    if (plen != 4) { s_resp[0] = RES_BAD_LENGTH; return 1; }
    uint16_t khz = get_u16(&p[0]);
    uint16_t ms  = get_u16(&p[2]);

    // 先に両方を検証し、どちらかが範囲外なら何も変えない
    bool khz_ok = (khz == 0 || khz == 100 || khz == 400);
    bool ms_ok  = (ms == 0 || (ms >= I2C_TIMEOUT_MIN_MS && ms <= I2C_TIMEOUT_MAX_MS));
    if (khz_ok && ms_ok) {
        if (khz) I2CH_SetSpeed(khz);
        if (ms)  I2CH_SetTimeout(ms);
        s_resp[0] = RES_OK;
    } else {
        s_resp[0] = RES_BAD_PARAM;
    }
    put_u16(&s_resp[1], I2CH_GetSpeed());
    put_u16(&s_resp[3], I2CH_GetTimeout());
    return 5;
}

static uint16_t cmd_probe(const uint8_t *p, uint16_t plen)
{
    if (plen != 1)    { s_resp[0] = RES_BAD_LENGTH; return 1; }
    if (p[0] > 0x7F)  { s_resp[0] = RES_BAD_PARAM;  return 1; }
    s_resp[0] = done(I2CH_Probe(p[0]));
    return 1;
}

static uint16_t cmd_scan(const uint8_t *p, uint16_t plen)
{
    uint8_t first = 0x08, last = 0x77;
    if (plen == 2)      { first = p[0]; last = p[1]; }
    else if (plen != 0) { s_resp[0] = RES_BAD_LENGTH; return 1; }
    if (first > last || last > 0x7F) { s_resp[0] = RES_BAD_PARAM; return 1; }

    uint8_t *bitmap = &s_resp[1];
    memset(bitmap, 0, 16);
    uint8_t status = RES_OK;

    for (uint8_t a = first; ; a++) {
        i2c_result_t r = done(I2CH_Probe(a));
        if (r == I2C_OK) {
            bitmap[a >> 3] |= (uint8_t)(1 << (a & 7));
        } else if (r != I2C_NACK_ADDR) {
            status = r;                 // バスの異常。ここで打ち切り、途中までの表を返す
            break;
        }
        wdt_reset();
        if (a == last) break;
    }
    s_resp[0] = status;
    return 17;
}

static uint16_t cmd_read(const uint8_t *p, uint16_t plen)
{
    if (plen != 3) { s_resp[0] = RES_BAD_LENGTH; return 1; }
    uint8_t  addr = p[0];
    uint16_t len  = get_u16(&p[1]);
    if (addr > 0x7F || len == 0 || len > MAX_XFER) { s_resp[0] = RES_BAD_PARAM; return 1; }

    uint16_t got;
    s_resp[0] = done(I2CH_Read(addr, &s_resp[3], len, &got));
    put_u16(&s_resp[1], got);
    return 3 + got;
}

static uint16_t cmd_write(const uint8_t *p, uint16_t plen)
{
    if (plen < 1 || plen - 1 > MAX_XFER) { s_resp[0] = RES_BAD_LENGTH; return 1; }
    if (p[0] > 0x7F)                     { s_resp[0] = RES_BAD_PARAM;  return 1; }

    uint16_t acked;
    s_resp[0] = done(I2CH_Write(p[0], &p[1], plen - 1, &acked));
    put_u16(&s_resp[1], acked);
    return 3;
}

static uint16_t cmd_write_read(const uint8_t *p, uint16_t plen)
{
    if (plen < 5) { s_resp[0] = RES_BAD_LENGTH; return 1; }
    uint8_t  addr = p[0];
    uint16_t wlen = get_u16(&p[1]);
    if (plen != 5 + wlen) { s_resp[0] = RES_BAD_LENGTH; return 1; }
    uint16_t rlen = get_u16(&p[3 + wlen]);
    if (addr > 0x7F || wlen == 0 || wlen > MAX_XFER || rlen == 0 || rlen > MAX_XFER) {
        s_resp[0] = RES_BAD_PARAM;
        return 1;
    }

    uint16_t got;
    s_resp[0] = done(I2CH_WriteRead(addr, &p[3], wlen, &s_resp[3], rlen, &got));
    put_u16(&s_resp[1], got);
    return 3 + got;
}

// BATCH の操作 1 つぶんの解析結果
typedef struct {
    uint8_t         type;
    uint8_t         addr;
    uint16_t        wlen;
    const uint8_t  *wdata;
    uint16_t        rlen;
} batch_op_t;

// p から操作を 1 つ読み、次の位置を返す。形が不正なら NULL。
static const uint8_t *parse_op(const uint8_t *p, const uint8_t *end, batch_op_t *op)
{
    if (end - p < 4) return NULL;
    op->type  = p[0];
    op->addr  = p[1];
    op->wlen  = 0;
    op->wdata = NULL;
    op->rlen  = 0;
    if (op->addr > 0x7F) return NULL;

    switch (op->type) {
        case CMD_READ:
            op->rlen = get_u16(&p[2]);
            if (op->rlen == 0 || op->rlen > MAX_XFER) return NULL;
            return p + 4;
        case CMD_WRITE:
            op->wlen  = get_u16(&p[2]);
            op->wdata = &p[4];
            if (op->wlen > MAX_XFER || (uint16_t)(end - op->wdata) < op->wlen) return NULL;
            return op->wdata + op->wlen;
        case CMD_WRITE_READ:
            op->wlen  = get_u16(&p[2]);
            op->wdata = &p[4];
            if (op->wlen == 0 || op->wlen > MAX_XFER
                || (uint16_t)(end - op->wdata) < (uint16_t)(op->wlen + 2)) return NULL;
            op->rlen = get_u16(op->wdata + op->wlen);
            if (op->rlen == 0 || op->rlen > MAX_XFER) return NULL;
            return op->wdata + op->wlen + 2;
        default:
            return NULL;
    }
}

static uint16_t cmd_batch(const uint8_t *p, uint16_t plen)
{
    if (plen < 2) { s_resp[0] = RES_BAD_LENGTH; return 1; }
    uint8_t flags = p[0];
    uint8_t count = p[1];
    if (count == 0 || count > MAX_BATCH_OPS) { s_resp[0] = RES_BAD_PARAM; return 1; }

    // 実行する前に全体を検証する。途中で形の誤りに気づいて半端に実行することを避け、
    // 応答のデータが MAX_BATCH に収まることもここで確かめる。
    const uint8_t *end = p + plen;
    const uint8_t *q   = p + 2;
    uint16_t total = 0;
    batch_op_t op;
    for (uint8_t i = 0; i < count; i++) {
        q = parse_op(q, end, &op);
        if (q == NULL) { s_resp[0] = RES_BAD_LENGTH; return 1; }
        total += op.rlen;
    }
    if (q != end || total > MAX_BATCH) { s_resp[0] = RES_BAD_LENGTH; return 1; }

    // 実行
    uint8_t  overall  = RES_OK;
    uint8_t  executed = 0;
    uint16_t n = 2;
    q = p + 2;
    for (uint8_t i = 0; i < count; i++) {
        q = parse_op(q, end, &op);
        uint8_t *res = &s_resp[n];
        uint16_t got = 0;
        i2c_result_t r;
        switch (op.type) {
            case CMD_READ:  r = I2CH_Read(op.addr, &res[3], op.rlen, &got); break;
            case CMD_WRITE: r = I2CH_Write(op.addr, op.wdata, op.wlen, &got); break;
            default:        r = I2CH_WriteRead(op.addr, op.wdata, op.wlen, &res[3], op.rlen, &got); break;
        }
        done(r);
        res[0] = r;
        put_u16(&res[1], got);
        n += 3 + ((op.type == CMD_WRITE) ? 0 : got);
        executed++;
        wdt_reset();

        if (r != I2C_OK) {
            if (overall == RES_OK) overall = r;
            if (flags & BATCH_STOP_ON_ERROR) break;
        }
    }
    s_resp[0] = overall;
    s_resp[1] = executed;
    return n;
}

static uint16_t cmd_bus_status(const uint8_t *p, uint16_t plen)
{
    (void)p; (void)plen;
    i2c_counters_t c;
    I2CH_GetCounters(&c);
    s_resp[0] = RES_OK;
    I2CH_GetPins(&s_resp[1], &s_resp[2]);
    s_resp[3] = I2CH_GetBusState();
    put_u16(&s_resp[4],  c.timeout);
    put_u16(&s_resp[6],  c.nack);
    put_u16(&s_resp[8],  c.busBusy);
    put_u16(&s_resp[10], c.arbLost);
    put_u16(&s_resp[12], c.recover);
    return 14;
}

static uint16_t cmd_bus_recover(const uint8_t *p, uint16_t plen)
{
    (void)p; (void)plen;
    s_resp[0] = I2CH_Recover();
    I2CH_GetPins(&s_resp[1], &s_resp[2]);
    return 3;
}

static uint16_t cmd_led(const uint8_t *p, uint16_t plen)
{
    if (plen != 2) { s_resp[0] = RES_BAD_LENGTH; return 1; }
    switch (p[0]) {
        case 0:  LED_Identify(0);    s_resp[0] = RES_OK; break;   // 自動に戻す
        case 1:  LED_Identify(p[1]); s_resp[0] = RES_OK; break;   // 識別
        default: s_resp[0] = RES_BAD_PARAM; break;
    }
    return 1;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="振り分け">

// 応答を USB のホストへ送り切るまで待ってから、ソフトウェアリセットする。
// 100 ms あれば数フレームぶんで、応答は十分に送り切れる。
static void reboot_after_flush(void)
{
    uint32_t until = SYSTIME_Millis() + 100;
    while (!SYSTIME_IsPast(until)) {
        tud_task();
        wdt_reset();
    }
    ccp_write_io((void *)&RSTCTRL.SWRR, RSTCTRL_SWRST_bm);
}

void BRIDGE_OnSysEx(uint8_t *msg, uint16_t len, bool truncated)
{
    // MIDI 標準の機器問い合わせ
    if (!truncated && handle_device_inquiry(msg, len)) return;

    // ヘッダ: <MFR> <PRODUCT> <SEQ> <CMD>。一致しなければ黙って捨てる (仕様 §3.1)。
    const uint16_t hdr = SYSEX_MFR_LEN + 3;
    if (len < hdr) return;
    if (msg[0] != SYSEX_MFR_0) return;
#if SYSEX_MFR_LEN == 3
    if (msg[1] != SYSEX_MFR_1 || msg[2] != SYSEX_MFR_2) return;
#endif
    if (msg[SYSEX_MFR_LEN] != SYSEX_PRODUCT) return;
    uint8_t seq = msg[SYSEX_MFR_LEN + 1];
    uint8_t cmd = msg[SYSEX_MFR_LEN + 2];
    if (cmd > 0x3F) return;                         // 応答の番号 (0x40 以上) は受け付けない

    if (truncated) { reply_status(seq, cmd, RES_BAD_LENGTH); return; }

    bool ok;
    uint8_t *p = &msg[hdr];
    uint16_t plen = SYSEX_Decode7(p, len - hdr, &ok);
    if (!ok) { reply_status(seq, cmd, RES_BAD_ENCODING); return; }

    uint16_t n;
    switch (cmd) {
        case CMD_GET_INFO:    n = cmd_get_info(p, plen);    break;
        case CMD_SET_CONFIG:  n = cmd_set_config(p, plen);  break;
        case CMD_PROBE:       n = cmd_probe(p, plen);       break;
        case CMD_SCAN:        n = cmd_scan(p, plen);        break;
        case CMD_READ:        n = cmd_read(p, plen);        break;
        case CMD_WRITE:       n = cmd_write(p, plen);       break;
        case CMD_WRITE_READ:  n = cmd_write_read(p, plen);  break;
        case CMD_BATCH:       n = cmd_batch(p, plen);       break;
        case CMD_BUS_STATUS:  n = cmd_bus_status(p, plen);  break;
        case CMD_BUS_RECOVER: n = cmd_bus_recover(p, plen); break;
        case CMD_LED:         n = cmd_led(p, plen);         break;
        case CMD_REBOOT:
            s_resp[0] = RES_OK;
            n = 1;
            s_reboot_requested = true;
            break;
        default:
            s_resp[0] = RES_UNKNOWN_CMD;
            n = 1;
            break;
    }
    reply(seq, cmd, n);

    if (s_reboot_requested) reboot_after_flush();
}

// </editor-fold>
