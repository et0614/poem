/*
 * File:   eeprom_manager.h
 * Author: e.togashi
 *
 * EEPROM への永続化機能。I2C アドレスと線形補正係数 (気圧・温度の a/b) と装置ラベルを保存する。
 */

#ifndef EEPROM_MANAGER_H
#define	EEPROM_MANAGER_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// EEPROM を読み込み、SharedMemory に反映する
void EM_loadEEPROM(void);

// SharedMemory の内容を EEPROM に書き戻す (差分のみ)
void EM_updateEEPROM(void);

// EEPROM をデフォルト値で初期化する
void EM_resetEEPROM(void);

#ifdef	__cplusplus
}
#endif

#endif	/* EEPROM_MANAGER_H */
