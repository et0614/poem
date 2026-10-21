#include "sysex.h"
#include "bridge.h"
#include "link_config.h"
#include "systime.h"
#include "tinyusb/tusb.h"

#include <avr/wdt.h>

// 送信バッファが空くのを待つ上限 [ms]。USB のホストが読み出しを止めた場合に、
// いつまでも待ち続けないため。
#define SEND_WAIT_MS    500

// <editor-fold defaultstate="collapsed" desc="受信">

typedef enum {
    RX_IDLE,        // F0 待ち
    RX_BODY,        // 本体を蓄積中
    RX_OVERFLOW,    // 長すぎる: F7 まで捨てる (先頭は残っているので応答はできる)
} rx_state_t;

static uint8_t    s_rx[SYSEX_RX_MAX];
static uint16_t   s_rx_len = 0;
static rx_state_t s_rx_state = RX_IDLE;

static void feed_byte(uint8_t b)
{
    if (b == 0xF0) {                    // 開始。途中のものは捨てる
        s_rx_len   = 0;
        s_rx_state = RX_BODY;
        return;
    }
    if (b == 0xF7) {                    // 終了
        if (s_rx_state != RX_IDLE) {
            BRIDGE_OnSysEx(s_rx, s_rx_len, s_rx_state == RX_OVERFLOW);
        }
        s_rx_state = RX_IDLE;
        return;
    }
    if (b & 0x80) {                     // F7 以外のステータスバイトは SysEx を打ち切る
        s_rx_state = RX_IDLE;
        return;
    }
    if (s_rx_state == RX_BODY) {
        if (s_rx_len < sizeof(s_rx)) s_rx[s_rx_len++] = b;
        else s_rx_state = RX_OVERFLOW;
    }
}

void SYSEX_Task(void)
{
    uint8_t packet[4];
    while (tud_midi_packet_read(packet)) {
        // 先頭バイトの下位 4bit が CIN (Code Index Number)。SysEx に関係するものだけ扱う。
        switch (packet[0] & 0x0F) {
            case 0x04:  // SysEx の開始または継続 (3 バイト)
            case 0x07:  // SysEx の終了 (3 バイト)
                feed_byte(packet[1]);
                feed_byte(packet[2]);
                feed_byte(packet[3]);
                break;
            case 0x06:  // SysEx の終了 (2 バイト)
                feed_byte(packet[1]);
                feed_byte(packet[2]);
                break;
            case 0x05:  // SysEx の終了 (1 バイト)
                feed_byte(packet[1]);
                break;
            default:    // ノートなどは無視
                break;
        }
    }
}

uint16_t SYSEX_Decode7(uint8_t *buf, uint16_t len, bool *ok)
{
    // 7 バイトごとに先頭へ「最上位ビットのまとめ (h)」が付いている。
    // 復号後の位置 o は常に読み出し位置 i より手前なので、その場で書き換えてよい。
    uint16_t i = 0, o = 0;
    *ok = true;
    while (i < len) {
        uint8_t  h = buf[i++];
        uint16_t n = len - i;
        if (n > 7) n = 7;
        if (n == 0) { *ok = false; return 0; }     // h だけで中身が無い
        for (uint8_t k = 0; k < n; k++) {
            buf[o++] = buf[i + k] | (uint8_t)(((h >> k) & 1) << 7);
        }
        i += n;
    }
    return o;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="送信">

// 送信中の 3 バイトの組 (USB-MIDI の 1 パケットぶん)
static uint8_t s_tx[3];
static uint8_t s_tx_n;
static bool    s_tx_ok;

// 送信バッファが空くまで tud_task() を回して待つ。
static void write_packet(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2)
{
    if (!s_tx_ok) return;
    const uint8_t pkt[4] = { cin, b0, b1, b2 };
    uint32_t give_up = SYSTIME_Millis() + SEND_WAIT_MS;
    while (!tud_midi_packet_write(pkt)) {
        if (!tud_midi_mounted() || SYSTIME_IsPast(give_up)) { s_tx_ok = false; return; }
        tud_task();
        wdt_reset();
    }
}

static void tx_begin(void)
{
    s_tx_n  = 0;
    s_tx_ok = tud_midi_mounted();
}

// F0 から F7 の手前までのバイトを 1 つ積む。3 つ揃ったら送る。
static void tx_put(uint8_t b)
{
    s_tx[s_tx_n++] = b;
    if (s_tx_n == 3) {
        write_packet(0x04, s_tx[0], s_tx[1], s_tx[2]);   // SysEx の開始または継続
        s_tx_n = 0;
    }
}

// F7 を付けて終える。残りのバイト数で CIN が変わる。
static bool tx_end(void)
{
    s_tx[s_tx_n++] = 0xF7;
    switch (s_tx_n) {
        case 1: write_packet(0x05, s_tx[0], 0, 0);             break;
        case 2: write_packet(0x06, s_tx[0], s_tx[1], 0);       break;
        default: write_packet(0x07, s_tx[0], s_tx[1], s_tx[2]); break;
    }
    // tud_midi_packet_write() は書き込んだ時点で送信を始めるので、別途の催促は要らない
    return s_tx_ok;
}

bool SYSEX_SendResponse(uint8_t seq, uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    tx_begin();
    tx_put(0xF0);
    tx_put(SYSEX_MFR_0);
#if SYSEX_MFR_LEN == 3
    tx_put(SYSEX_MFR_1);
    tx_put(SYSEX_MFR_2);
#endif
    tx_put(SYSEX_PRODUCT);
    tx_put(seq & 0x7F);
    tx_put((uint8_t)((cmd | 0x40) & 0x7F));

    // 7 バイトごとに、最上位ビットのまとめ h を先に送る
    for (uint16_t i = 0; i < len; i += 7) {
        uint16_t n = len - i;
        if (n > 7) n = 7;
        uint8_t h = 0;
        for (uint8_t k = 0; k < n; k++) {
            if (payload[i + k] & 0x80) h |= (uint8_t)(1 << k);
        }
        tx_put(h);
        for (uint8_t k = 0; k < n; k++) tx_put(payload[i + k] & 0x7F);
    }
    return tx_end();
}

bool SYSEX_SendRaw(const uint8_t *msg, uint16_t len)
{
    if (len < 2) return false;
    tx_begin();
    for (uint16_t i = 0; i + 1 < len; i++) tx_put(msg[i]);   // 末尾の F7 は tx_end が付ける
    return tx_end();
}

// </editor-fold>
