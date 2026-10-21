/*
 * File:   i2c_host.h
 * Author: e.togashi
 *
 * 汎用 I2C 層 (仕様の L0〜L1)。TWI0 のレジスタを直接操作する。
 * 相手が何の機器かは一切解釈しない。OSL の扱いは上位 (C# / Python) が行う。
 */

#ifndef I2C_HOST_H
#define	I2C_HOST_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// 転送の結果。値は SysEx の結果コード (仕様 §5) と同じにしてあり、そのまま返せる。
typedef enum {
    I2C_OK        = 0x00,   // 成功
    I2C_NACK_ADDR = 0x01,   // アドレスに応答が無い
    I2C_NACK_DATA = 0x02,   // 書き込みの途中で NACK された
    I2C_TIMEOUT   = 0x03,   // 時間内に終わらなかった
    I2C_BUS_BUSY  = 0x04,   // 開始前にバスが使用中だった
    I2C_ARB_LOST  = 0x05,   // 調停に負けた
    I2C_BUS_STUCK = 0x06,   // SDA / SCL が Low のまま復帰できなかった
    I2C_BUS_ERROR = 0x07,   // 不正な START / STOP を検出した
} i2c_result_t;

// 異常の回数 (起動からの累計、一巡する)
typedef struct {
    uint16_t timeout;
    uint16_t nack;
    uint16_t busBusy;
    uint16_t arbLost;
    uint16_t recover;
} i2c_counters_t;

// 初期化。MCC の TWI0_Initialize() の後に呼ぶ。
void I2CH_Init(void);

// 速度 [kHz] (100 または 400)。範囲外は false で、設定は変えない。
bool     I2CH_SetSpeed(uint16_t khz);
uint16_t I2CH_GetSpeed(void);

// 1 回の転送のタイムアウト [ms] (10〜1000)。範囲外は false で、設定は変えない。
bool     I2CH_SetTimeout(uint16_t ms);
uint16_t I2CH_GetTimeout(void);

// 機器がいるかを調べる。S, adr+R で始め、ACK なら 1 バイト読んで NACK と STOP で
// 終える。書き込みを伴わないので、正体の分からない機器にも安全。
i2c_result_t I2CH_Probe(uint8_t addr);

// S, adr+R, data…, P。got には読めたバイト数を返す。
i2c_result_t I2CH_Read(uint8_t addr, uint8_t *buf, uint16_t len, uint16_t *got);

// S, adr+W, data…, P。len = 0 ならアドレスだけを送る (Quick Write)。
// acked には ACK されたバイト数を返す。途中で NACK されたらそこで STOP する。
i2c_result_t I2CH_Write(uint8_t addr, const uint8_t *data, uint16_t len, uint16_t *acked);

// S, adr+W, wdata…, Sr, adr+R, rdata…, P。got には読めたバイト数を返す。
i2c_result_t I2CH_WriteRead(uint8_t addr, const uint8_t *wdata, uint16_t wlen,
                            uint8_t *rdata, uint16_t rlen, uint16_t *got);

// バス固着からの復帰を試みる (仕様 §6.3)。成功なら I2C_OK、だめなら I2C_BUS_STUCK。
i2c_result_t I2CH_Recover(void);

// 直近の復帰で解けなかった (バスが固着したまま) なら true。LED の表示に使う。
bool I2CH_IsStuck(void);

// 診断用 (BUS_STATUS)
void    I2CH_GetPins(uint8_t *sda, uint8_t *scl);
uint8_t I2CH_GetBusState(void);    // 0: 不明、1: 空き、2: 自分が保持、3: 使用中
void    I2CH_GetCounters(i2c_counters_t *c);

#ifdef	__cplusplus
}
#endif

#endif	/* I2C_HOST_H */
