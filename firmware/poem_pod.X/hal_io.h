/* 
 * File:   hal_io.h
 * Author: e.togashi
 *
 * Created on April 27, 2026, 12:38 PM
 */

#ifndef HAL_IO_H
#define	HAL_IO_H

#ifdef	__cplusplus
extern "C" {
#endif

#include "mcc_generated_files/system/system.h"
#include "mcc_generated_files/system/clock.h" //F_CPUの設定
#include <util/delay.h>

//LED点滅設定
#define LED_ON_DURATION_MS  100 // 点灯時間
#define LED_OFF_DURATION_MS 25  // 消灯時間
    
static inline void turnOnGreenLED(void)
{
    IO_GLED_SetHigh();
}

static inline void turnOffGreenLED(void)
{
    IO_GLED_SetLow();
}

static inline void turnOnRedLED(void)
{
    IO_RLED_SetHigh();
}

static inline void turnOffRedLED(void)
{
    IO_RLED_SetLow();
}

static inline void blinkGreenLED(int iterNum)
{
	if(iterNum < 1) return;

	// 一度消灯してから指定回数の点滅
    turnOffGreenLED();
	for(int i=0; i < iterNum; i++)
	{
		_delay_ms(LED_ON_DURATION_MS);
        turnOnGreenLED();
		_delay_ms(LED_OFF_DURATION_MS);
        turnOffGreenLED();
	}
}

static inline void blinkGreenAndRedLED(int iterNum)
{
	if(iterNum < 1) return;

	// 一度消灯してから指定回数の点滅
    turnOffGreenLED();
    turnOffRedLED();
	for(int i=0; i < iterNum; i++)
	{
		_delay_ms(LED_ON_DURATION_MS);
        turnOnGreenLED();
        turnOnRedLED();
		_delay_ms(LED_OFF_DURATION_MS);
        turnOffGreenLED();
        turnOffRedLED();
	}
}

static inline void blinkRedLED(int iterNum)
{
	if(iterNum < 1) return;

	// 一度消灯してから指定回数の点滅
    turnOffRedLED();
	for(int i=0; i < iterNum; i++)
	{
		_delay_ms(LED_ON_DURATION_MS);
        turnOnRedLED();
		_delay_ms(LED_OFF_DURATION_MS);
        turnOffRedLED();
	}
}


#ifdef	__cplusplus
}
#endif

#endif	/* HAL_IO_H */

