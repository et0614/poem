/* 
 * File:   eeprom_manager.h
 * Author: etoga
 *
 * Created on April 27, 2026, 2:10 PM
 */

#ifndef EEPROM_MANAGER_H
#define	EEPROM_MANAGER_H

#ifdef	__cplusplus
extern "C" {
#endif
    
// EEPROMに保存するネットワーク設定用の構造体
typedef struct{
	uint16_t version;       // 設定フォーマットのバージョン
	uint8_t ip[4];          // IPアドレス
	uint8_t sn[4];          // サブネットマスク
	uint8_t gw[4];          // ゲートウェイ
	uint8_t dns[4];         // DNSサーバー
	uint8_t bip[4];         // ブローカーのIPアドレス
	char topic_suffix[21];  // トピック名(20 words+\0)
	uint8_t crc; //CRC
} EM_NetworkSettings;


extern EM_NetworkSettings EM_Network_Settings;

extern uint8_t EM_Is_First_Run;
    
//EEPROMを読み込む
void EM_loadEEPROM();

//設定を保存する
void EM_updateEEPROM();

//EEPROMを初期化する
void EM_resetEEPROM();


#ifdef	__cplusplus
}
#endif

#endif	/* EEPROM_MANAGER_H */

