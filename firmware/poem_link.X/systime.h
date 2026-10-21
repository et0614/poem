/*
 * File:   systime.h
 * Author: e.togashi
 *
 * 1 ms 刻みの時刻。TCA0 のオーバーフロー割り込みで進める (実体は main.c)。
 */

#ifndef SYSTIME_H
#define	SYSTIME_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// 起動からの経過時間 [ms]。約 49 日で一巡する。
uint32_t SYSTIME_Millis(void);

// deadline を過ぎたら true。一巡しても正しく比べられるよう差の符号で判定する。
static inline bool SYSTIME_IsPast(uint32_t deadline)
{
    return (int32_t)(SYSTIME_Millis() - deadline) >= 0;
}

#ifdef	__cplusplus
}
#endif

#endif	/* SYSTIME_H */
