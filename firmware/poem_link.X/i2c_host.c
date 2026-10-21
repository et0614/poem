#include "i2c_host.h"
#include "link_config.h"
#include "systime.h"
#include "mcc_generated_files/i2c_host/twi0.h"

#include <avr/io.h>
#include "mcc_generated_files/system/clock.h"   // F_CPU (_delay_us が要求する)
#include <util/delay.h>

// TWI0 の既定ピン: PA2 = SDA, PA3 = SCL (回路図どおり)
#define I2C_SDA_bm  PIN2_bm
#define I2C_SCL_bm  PIN3_bm

static uint16_t       s_khz        = I2C_DEFAULT_KHZ;
static uint16_t       s_timeout_ms = I2C_DEFAULT_TIMEOUT_MS;
static uint32_t       s_deadline;           // 実行中の転送の打ち切り時刻
static bool           s_stuck      = false; // 直近の復帰で解けなかった
static i2c_counters_t s_cnt;

// <editor-fold defaultstate="collapsed" desc="速度">

// データシートの式: MBAUD = (fCLK_PER / fSCL - 10 - fCLK_PER × tRISE) / 2
// tRISE は MCC と同じ 0.1 µs を見込む。12 MHz なら 100 kHz で 54、400 kHz で 9。
static uint8_t calc_mbaud(uint16_t khz)
{
    int32_t v = ((int32_t)(F_CPU / ((uint32_t)khz * 1000UL)) - 10
                 - (int32_t)(F_CPU / 10000000UL)) / 2;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

// MBAUD は host を止めているあいだに書く。再開したら BUSSTATE を IDLE にする
// (有効化した直後は UNKNOWN で、そのままでは転送を始められない)。
static void apply_speed(void)
{
    TWI0.MCTRLA &= (uint8_t)~TWI_ENABLE_bm;
    TWI0.MBAUD   = calc_mbaud(s_khz);
    TWI0.MCTRLA |= TWI_ENABLE_bm;
    TWI0.MSTATUS = TWI_BUSSTATE_IDLE_gc;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="バスの復帰">

// バスを物理的に解放させる (I2C の標準手順)。poem_pod.X の i2c_master.c と同じ。
//
// 子機は「クロックを送ってもらう」ことと「STOP を受け取ること」でしか状態機械を
// 進められない。TWI を切り離し、SCL を GPIO として叩いて残りのビットを吐き出させ、
// 最後に手で STOP を作る。
//
// OUT は 0 に固定し DIR だけを操作して、オープンドレインを手で再現する
// (Low = 出力 / High = 入力でプルアップに任せる)。High を駆動すると、子機が
// クロックを伸ばしている最中に電流を流し合うことになる。
static void bus_clear(void)
{
    PORTA.OUTCLR = I2C_SDA_bm | I2C_SCL_bm;
    PORTA.DIRCLR = I2C_SDA_bm | I2C_SCL_bm;
    _delay_us(5);

    // 最大 9 発。1 バイト + ACK ぶんに相当する。
    for (uint8_t i = 0; i < 9; i++) {
        if (PORTA.IN & I2C_SDA_bm) break;   // 解放された
        PORTA.DIRSET = I2C_SCL_bm;          // SCL Low
        _delay_us(5);
        PORTA.DIRCLR = I2C_SCL_bm;          // SCL High (解放)
        _delay_us(5);
    }

    // STOP を作る。SDA を下げるのは SCL が Low のあいだに行う。SCL が High の
    // ときに下げると START になってしまう。
    PORTA.DIRSET = I2C_SCL_bm;              // SCL Low
    _delay_us(5);
    PORTA.DIRSET = I2C_SDA_bm;              // SDA Low
    _delay_us(5);
    PORTA.DIRCLR = I2C_SCL_bm;              // SCL High
    _delay_us(5);
    PORTA.DIRCLR = I2C_SDA_bm;              // SDA High = STOP
    _delay_us(5);
}

static bool pins_released(void)
{
    uint8_t pins = PORTA.IN;
    return (pins & I2C_SDA_bm) && (pins & I2C_SCL_bm);
}

i2c_result_t I2CH_Recover(void)
{
    s_cnt.recover++;

    TWI0.MCTRLA = 0;            // host を止めてピンを GPIO 側へ渡す
    bus_clear();
    TWI0_Initialize();          // MCC の初期化 (レジスタを既定に戻す)
    apply_speed();              // 現在の速度を戻し、BUSSTATE を IDLE にする

    s_stuck = !pins_released();
    return s_stuck ? I2C_BUS_STUCK : I2C_OK;
}

bool I2CH_IsStuck(void)
{
    return s_stuck;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="内部: 転送の部品">

// 転送の後始末。結果に応じて回数を数え、必要なら復帰する。
//
// タイムアウトと BUS_ERROR は host の状態機械が中途半端なまま残るので、復帰させる。
// ARB_LOST はバスを別のマスタが使っているので、こちらは手を引くだけにする
// (復帰で SCL を叩くと相手の通信を壊す)。
static i2c_result_t finish(i2c_result_t r)
{
    switch (r) {
        case I2C_NACK_ADDR:
        case I2C_NACK_DATA:
            s_cnt.nack++;
            TWI0.MCTRLB = TWI_MCMD_STOP_gc;
            break;
        case I2C_TIMEOUT:
            s_cnt.timeout++;
            if (I2CH_Recover() != I2C_OK) r = I2C_BUS_STUCK;
            break;
        case I2C_BUS_ERROR:
            if (I2CH_Recover() != I2C_OK) r = I2C_BUS_STUCK;
            break;
        case I2C_ARB_LOST:
            s_cnt.arbLost++;
            TWI0.MSTATUS = TWI_ARBLOST_bm | TWI_BUSERR_bm;   // フラグを落とす
            break;
        default:
            break;
    }
    return r;
}

// 転送の開始前にバスの空きを確かめる。
//
// BUSSTATE が使用中なら I2C_IDLE_WAIT_MS まで待つ。別のマスタ (動作中の Pod など)
// がいる可能性があるので、空かなければ BUS_BUSY で諦める。
// SDA / SCL が Low に張り付いているのに BUSSTATE が IDLE のままのこともある
// (STOP を観測していないため)。その場合は 1 回だけ復帰を試みる。
static i2c_result_t begin(void)
{
    s_deadline = SYSTIME_Millis() + s_timeout_ms;

    uint32_t wait_until = SYSTIME_Millis() + I2C_IDLE_WAIT_MS;
    while (true) {
        uint8_t st = TWI0.MSTATUS & TWI_BUSSTATE_gm;
        if (st == TWI_BUSSTATE_UNKNOWN_gc) {
            TWI0.MSTATUS = TWI_BUSSTATE_IDLE_gc;
            continue;
        }
        if (st == TWI_BUSSTATE_IDLE_gc && pins_released()) return I2C_OK;
        if (SYSTIME_IsPast(wait_until)) break;
    }

    if ((TWI0.MSTATUS & TWI_BUSSTATE_gm) == TWI_BUSSTATE_BUSY_gc) {
        s_cnt.busBusy++;
        return I2C_BUS_BUSY;
    }

    // IDLE なのにピンが Low のまま → 固着。復帰を 1 回だけ試みる。
    if (I2CH_Recover() != I2C_OK) return I2C_BUS_STUCK;
    s_deadline = SYSTIME_Millis() + s_timeout_ms;
    return I2C_OK;
}

// 指定したフラグが立つまで待つ。ARBLOST / BUSERR を先に確かめる。
static i2c_result_t wait_flags(uint8_t mask)
{
    while (true) {
        uint8_t st = TWI0.MSTATUS;
        if (st & TWI_ARBLOST_bm) return I2C_ARB_LOST;
        if (st & TWI_BUSERR_bm)  return I2C_BUS_ERROR;
        if (st & mask)           return I2C_OK;
        if (SYSTIME_IsPast(s_deadline)) return I2C_TIMEOUT;
    }
}

// アドレス + W を送り、続けてデータを送る。STOP は送らない (呼び出し側が決める)。
static i2c_result_t write_phase(uint8_t addr, const uint8_t *data, uint16_t len, uint16_t *acked)
{
    *acked = 0;
    TWI0.MADDR = (uint8_t)(addr << 1);
    i2c_result_t r = wait_flags(TWI_WIF_bm);
    if (r != I2C_OK) return r;
    if (TWI0.MSTATUS & TWI_RXACK_bm) return I2C_NACK_ADDR;

    for (uint16_t i = 0; i < len; i++) {
        TWI0.MDATA = data[i];
        r = wait_flags(TWI_WIF_bm);
        if (r != I2C_OK) return r;
        if (TWI0.MSTATUS & TWI_RXACK_bm) return I2C_NACK_DATA;
        (*acked)++;
    }
    return I2C_OK;
}

// アドレス + R を送り、len バイト読む。書き込みの後に呼べば Repeated START になる。
// 最後のバイトには NACK を返し、STOP で終える。
//
// SMEN (Smart Mode) は無効なので、1 バイト読むたびに MCMD を書いて次へ進める。
// 応答 (ACK / NACK) は MCMD を書いた時点で送られる。
static i2c_result_t read_phase(uint8_t addr, uint8_t *buf, uint16_t len, uint16_t *got)
{
    *got = 0;
    TWI0.MADDR = (uint8_t)((addr << 1) | 1);

    // アドレスに ACK が返ると最初のバイトの受信まで進んで RIF が立つ。
    // NACK なら WIF が立つ。
    i2c_result_t r = wait_flags(TWI_RIF_bm | TWI_WIF_bm);
    if (r != I2C_OK) return r;
    if (TWI0.MSTATUS & TWI_WIF_bm) return I2C_NACK_ADDR;

    for (uint16_t i = 0; i < len; i++) {
        if (i > 0) {
            r = wait_flags(TWI_RIF_bm);
            if (r != I2C_OK) return r;
        }
        buf[i] = TWI0.MDATA;
        (*got)++;
        if (i + 1 < len) {
            TWI0.MCTRLB = TWI_MCMD_RECVTRANS_gc;                 // ACK して次を受信
        } else {
            TWI0.MCTRLB = TWI_ACKACT_bm | TWI_MCMD_STOP_gc;      // NACK して STOP
        }
    }
    return I2C_OK;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="公開関数">

void I2CH_Init(void)
{
    s_khz        = I2C_DEFAULT_KHZ;
    s_timeout_ms = I2C_DEFAULT_TIMEOUT_MS;
    apply_speed();
    s_stuck = !pins_released();
}

bool I2CH_SetSpeed(uint16_t khz)
{
    if (khz != 100 && khz != 400) return false;
    s_khz = khz;
    apply_speed();
    return true;
}

uint16_t I2CH_GetSpeed(void)
{
    return s_khz;
}

bool I2CH_SetTimeout(uint16_t ms)
{
    if (ms < I2C_TIMEOUT_MIN_MS || ms > I2C_TIMEOUT_MAX_MS) return false;
    s_timeout_ms = ms;
    return true;
}

uint16_t I2CH_GetTimeout(void)
{
    return s_timeout_ms;
}

i2c_result_t I2CH_Probe(uint8_t addr)
{
    uint8_t  dummy;
    uint16_t got;
    return I2CH_Read(addr, &dummy, 1, &got);
}

i2c_result_t I2CH_Read(uint8_t addr, uint8_t *buf, uint16_t len, uint16_t *got)
{
    *got = 0;
    i2c_result_t r = begin();
    if (r != I2C_OK) return r;
    return finish(read_phase(addr, buf, len, got));
}

i2c_result_t I2CH_Write(uint8_t addr, const uint8_t *data, uint16_t len, uint16_t *acked)
{
    *acked = 0;
    i2c_result_t r = begin();
    if (r != I2C_OK) return r;

    r = write_phase(addr, data, len, acked);
    if (r == I2C_OK) TWI0.MCTRLB = TWI_MCMD_STOP_gc;
    return finish(r);
}

i2c_result_t I2CH_WriteRead(uint8_t addr, const uint8_t *wdata, uint16_t wlen,
                            uint8_t *rdata, uint16_t rlen, uint16_t *got)
{
    *got = 0;
    i2c_result_t r = begin();
    if (r != I2C_OK) return r;

    uint16_t acked;
    r = write_phase(addr, wdata, wlen, &acked);
    if (r == I2C_OK) r = read_phase(addr, rdata, rlen, got);   // Repeated START
    return finish(r);
}

void I2CH_GetPins(uint8_t *sda, uint8_t *scl)
{
    uint8_t pins = PORTA.IN;
    *sda = (pins & I2C_SDA_bm) ? 1 : 0;
    *scl = (pins & I2C_SCL_bm) ? 1 : 0;
}

uint8_t I2CH_GetBusState(void)
{
    return TWI0.MSTATUS & TWI_BUSSTATE_gm;
}

void I2CH_GetCounters(i2c_counters_t *c)
{
    *c = s_cnt;
}

// </editor-fold>
