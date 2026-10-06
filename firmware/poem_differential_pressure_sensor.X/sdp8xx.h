/*
 * File:   sdp8xx.h
 * Author: e.togashi
 *
 * Sensirion SDP8xx-Digital (SDP810-125Pa) 差圧センサのドライバ。
 * 連続測定モード (差圧温度補償・average till read) で動かし、結果を読むだけにする。
 */

#ifndef SDP8XX_H
#define	SDP8XX_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// SDP8xx の I2C アドレス (固定)
#define SDP8XX_ADDR     (0x25)

// 連続測定を開始する。すでに測定中でもよい (先に停止してから開始し直す)。
// 開始後 8 ms は結果が無く、読み出しは NACK で失敗する。
bool SDP8xx_Initialize(void);

// 最新の差圧 [Pa] と温度 [°C] を読む。CRC 不一致や NACK で false。
// 差圧の換算係数はセンサが返す値を使う (125Pa 品は 240、500Pa 品は 60)。
bool SDP8xx_ReadValue(float *dpValue, float *tempValue);

#ifdef	__cplusplus
}
#endif

#endif	/* SDP8XX_H */
