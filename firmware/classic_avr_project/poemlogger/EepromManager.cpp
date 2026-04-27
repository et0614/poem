/**
 * @file EepromManager.h
 * @brief AVR(AVR128DB32)のEEPROMを処理する
 * @author E.Togashi
 * @date 2025.06.21
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <avr/eeprom.h>

#include "EepromManager.h"
#include "Utilities.h"

//ネットワーク設定
static NetworkSettings EEMEM EEP_NET_SETTING;

//温湿度補正係数
static CorrectionFactors EEMEM EEP_CFACTORS[EepromManager::SENSOR_TYPE_COUNT];

//風速特性係数
static VelocityCharacteristicCoefficients EEMEM EEP_VCCOEFS[4];

//ネットワーク設定
NetworkSettings EepromManager::net_settings;

//温湿度補正係数
CorrectionFactors EepromManager::cFactors[EepromManager::SENSOR_TYPE_COUNT];

//風速特性係数
VelocityCharacteristicCoefficients EepromManager::vcCoefficients[4];

void EepromManager::initCorrectionFactors(SensorType type) {
	if (type >= SENSOR_TYPE_COUNT) return; // 範囲外の型が来たら何もしない

	CorrectionFactors initialFactors;
	initialFactors.version = 1; // バージョンを設定
	// 5つのセンサーの補正係数をループで初期化 (a=1.0, b=0.0)
	for (int i = 0; i < 5; ++i) {
		initialFactors.coeffs[i].temp_a = 1.0f;
		initialFactors.coeffs[i].temp_b = 0.0f;
		initialFactors.coeffs[i].humi_a = 1.0f;
		initialFactors.coeffs[i].humi_b = 0.0f;
	}
	initialFactors.crc = 0; //一旦0

	// デフォルト値を設定
	cFactors[type] = initialFactors;

	// CRCを計算
	cFactors[type].crc = Utilities::crc8(
		(uint8_t*)&cFactors[type],
		sizeof(CorrectionFactors) - sizeof(cFactors[type].crc)
		);

	// EEPROM更新
	eeprom_busy_wait();
	eeprom_update_block(&cFactors[type], &EEP_CFACTORS[type], sizeof(CorrectionFactors));
}

void EepromManager::initVCCoefficients(uint8_t index){
	EepromManager::vcCoefficients[index] = {
		1,		//バージョン
		1.50,	//無風電圧
		2.730,	//係数A
		128.0,	//係数B
		0		//CRC（一旦0で初期化）
	};
	// CRCを計算
	EepromManager::vcCoefficients[index].crc = Utilities::crc8(
		(uint8_t*)&EepromManager::vcCoefficients[index],
		sizeof(VelocityCharacteristicCoefficients) - sizeof(EepromManager::vcCoefficients[index].crc) //crcメンバー自身のサイズは計算範囲から除外する
	);
	//EEPROM更新
	eeprom_busy_wait();
	eeprom_update_block(&EepromManager::vcCoefficients[index], (void*)&EEP_VCCOEFS[index], sizeof(VelocityCharacteristicCoefficients));
}

// ネットワーク設定をEEPROMに保存する
void EepromManager::SetNetworkSettings(const NetworkSettings& settings) {
	EepromManager::net_settings = settings;
	eeprom_busy_wait();
	eeprom_update_block(&settings, (void*)&EEP_NET_SETTING, sizeof(NetworkSettings));
}

void EepromManager::InitializeNetworkSettings(void){
	NetworkSettings settings = {
		1,                      // version
		/*{192, 168, 11, 99},     // ip
		{255, 255, 255, 0},     // sn
		{192, 168, 11, 1},      // gw
		{8, 8, 8, 8},           // dns
		{192, 168, 11, 14},     // ブローカーのip*/
		/*{172, 22, 1, 201},     // ip
		{255, 255, 248, 0},     // sn
		{172, 22, 0, 1},      // gw
		{8, 8, 8, 8},           // dns
		{172, 22, 1, 95},     // ブローカーのip*/
		{192, 168, 1, 20},     // ip
		{255, 255, 255, 0},     // sn
		{192, 168, 1, 20},      // gw
		{8, 8, 8, 8},           // dns
		{192, 168, 1, 10},     // ブローカーのip
		"my_device",				// トピック名
		0		//CRC（一旦0で初期化）
	};
	// CRCを計算
	settings.crc = Utilities::crc8(
		(uint8_t*)&settings,
		sizeof(NetworkSettings) - sizeof(settings.crc) //crcメンバー自身のサイズは計算範囲から除外する
	);
	EepromManager::net_settings = settings;
	//EEPROM更新
	eeprom_busy_wait();
	eeprom_update_block(&EepromManager::net_settings, (void*)&EEP_NET_SETTING, sizeof(NetworkSettings));
}

// EEPROMからネットワーク設定を読み出す
void EepromManager::loadNetworkSettings() {
	eeprom_busy_wait();
	eeprom_read_block(&EepromManager::net_settings, (const void*)&EEP_NET_SETTING, sizeof(NetworkSettings));
	
	// 読み込んだデータのCRCを検証
	uint8_t expected_crc = EepromManager::net_settings.crc;
	uint8_t actual_crc = Utilities::crc8(
		(uint8_t*)&EepromManager::net_settings,
		sizeof(NetworkSettings) - sizeof(EepromManager::net_settings.crc)
	);

	// CRCが一致しない（データ破損）場合にはデフォルト値で再初期化
	if (expected_crc != actual_crc) EepromManager::InitializeNetworkSettings();
}

//補正係数をEEPROMに保存する
void EepromManager::SetCorrectionFactors(SensorType type, const CorrectionFactors &factors) {
	if (type >= SENSOR_TYPE_COUNT) return;

	cFactors[type] = factors; // RAM上の変数を更新
	eeprom_busy_wait();
	// EEPROM上の対応する場所を更新
	eeprom_update_block(&cFactors[type], &EEP_CFACTORS[type], sizeof(CorrectionFactors));
}

//補正係数を読み出す
void EepromManager::loadCorrectionFactors(EepromManager::SensorType type) {
	if (type >= EepromManager::SENSOR_TYPE_COUNT) return;
	
	eeprom_busy_wait();
	// EEPROMの対応する場所から読み込み
	eeprom_read_block(&EepromManager::cFactors[type], &EEP_CFACTORS[type], sizeof(CorrectionFactors));

	// CRC検証
	uint8_t expected_crc = EepromManager::cFactors[type].crc;
	uint8_t actual_crc = Utilities::crc8(
		(uint8_t*)&EepromManager::cFactors[type],
		sizeof(CorrectionFactors) - sizeof(EepromManager::cFactors[type].crc)
		);

	// CRCが一致しない場合はデフォルト値で再初期化
	if (expected_crc != actual_crc) {
		EepromManager::initCorrectionFactors(type);
	}
}

// 風速特性係数をEEPROMに保存する
void EepromManager::SetVCCoefficients(uint8_t index, const VelocityCharacteristicCoefficients &vcCoefficients){
	EepromManager::vcCoefficients[index] = vcCoefficients;
	eeprom_busy_wait();
	eeprom_update_block(&vcCoefficients, (void*)&EEP_VCCOEFS[index], sizeof(VelocityCharacteristicCoefficients));
}

void EepromManager::loadVCCoefficients(uint8_t index){
	eeprom_busy_wait();
	eeprom_read_block(&EepromManager::vcCoefficients[index], (const void*)&EEP_VCCOEFS[index], sizeof(VelocityCharacteristicCoefficients));
	
	// 読み込んだデータのCRCを検証
	uint8_t expected_crc = EepromManager::vcCoefficients[index].crc;
	uint8_t actual_crc = Utilities::crc8(
		(uint8_t*)&EepromManager::vcCoefficients[index],
		sizeof(VelocityCharacteristicCoefficients) - sizeof(EepromManager::vcCoefficients[index].crc)
	);

	// CRCが一致しない（データ破損）場合にはデフォルト値で再初期化
	if (expected_crc != actual_crc) EepromManager::initVCCoefficients(index);
}

//設定を読み込む
void EepromManager::LoadEEPROM()
{
	//それぞれの構造体を読み込む。失敗すると自動で初期化される
	loadNetworkSettings();
	for (int i = 0; i < SENSOR_TYPE_COUNT; ++i)
		EepromManager::loadCorrectionFactors(static_cast<SensorType>(i));
	for (int i = 0; i < 4; ++i)
		loadVCCoefficients(i);	
}
