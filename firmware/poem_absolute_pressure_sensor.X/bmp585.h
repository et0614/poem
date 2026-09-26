/*
 * File:   bmp585.h
 * Author: e.togashi
 *
 * Bosch Sensortec BMP585 絶対圧センサのドライバ。
 * NORMAL モード (10 Hz) で動かし、最新の測定結果をデータレジスタから読むだけにする。
 */

#ifndef BMP585_H
#define	BMP585_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// BMP585 の I2C アドレス。SDO = GND で 0x46、SDO = VDDIO で 0x47。
// 基板では SDO (と CSB) を VDDIO に直結している。
#define BMP585_ADDR     (0x47)

// ソフトリセットから測定開始までを行う。すでに測定中でもよい (リセットで止まる)。
// CHIP_ID / NVM 状態 / OSR・ODR の組み合わせのいずれかが異常なら false。
bool BMP585_Initialize(void);

// 最新の気圧 [Pa] と温度 [°C] を読む。通信失敗や範囲外の値で false。
bool BMP585_ReadValue(float *pressValue, float *tempValue);

// センサが自分で再起動していないかを調べる (INT_STATUS.por)。
// 再起動していれば設定が既定値 (測定停止) に戻っているので、呼び出し側で
// BMP585_Initialize() をやり直す。読み出しに失敗した場合も true を返す。
bool BMP585_NeedsRestart(void);

#ifdef	__cplusplus
}
#endif

#endif	/* BMP585_H */
