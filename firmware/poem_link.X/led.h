/*
 * File:   led.h
 * Author: e.togashi
 *
 * 状態表示 LED (PA1、High で点灯)。表示の決まりは仕様 §7。
 */

#ifndef LED_H
#define	LED_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>

// メインループから毎回呼ぶ。現在の状態に応じて点灯・消灯を決める。
void LED_Task(void);

// I2C の転送を 1 回行ったことを知らせる (短く消える)。
void LED_Activity(void);

// 識別表示: seconds 秒のあいだ 2 Hz で点滅させる。0 なら止めて自動に戻す。
void LED_Identify(uint8_t seconds);

#ifdef	__cplusplus
}
#endif

#endif	/* LED_H */
