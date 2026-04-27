/**
 * @file EepromManager.h
 * @brief AVR(AVR128DB32)のEEPROMを処理する
 * @author E.Togashi
 * @date 2025.06.21
 */

#ifndef EEPROM_MANAGER_H_
#define EEPROM_MANAGER_H_

// EEPROMに保存するネットワーク設定用の構造体
struct NetworkSettings {
	uint16_t version;       // 設定フォーマットのバージョン
	uint8_t ip[4];          // IPアドレス
	uint8_t sn[4];          // サブネットマスク
	uint8_t gw[4];          // ゲートウェイ
	uint8_t dns[4];         // DNSサーバー
	uint8_t bip[4];         // ブローカーのIPアドレス
	char topic_suffix[21];  // トピック名(20 words+\0)
	uint8_t crc; //CRC
};

//温湿度センサの補正係数
struct PerSensorCoefficients {
	float temp_a;
	float temp_b;
	float humi_a;
	float humi_b;
};

// センサの補正係数
struct CorrectionFactors {
	uint16_t version; //バージョン
	PerSensorCoefficients coeffs[5]; //温湿度センサ5つで1セット
	uint8_t crc; //CRC
};

// 風速特性係数
struct VelocityCharacteristicCoefficients{
	uint16_t version; //バージョン
	float vol0;
	float ccA;
	float ccB;
	uint8_t crc; //CRC
};

class EepromManager
{
	public:
		/**
		 * @brief SHT4Xの種類
		 */
		enum SensorType
		{
			A_type = 0,
			B_type,
			C_type,
			SENSOR_TYPE_COUNT
		};
		
		//ネットワーク設定
		static NetworkSettings net_settings;
		
		//補正係数
		static CorrectionFactors cFactors[SENSOR_TYPE_COUNT];

		//風速特性係数
		static VelocityCharacteristicCoefficients vcCoefficients[4];
		
		// ネットワーク設定をEEPROMに保存する
		static void SetNetworkSettings(const NetworkSettings& settings);
		
		// ネットワーク設定を初期化する
		static void InitializeNetworkSettings(void);
		
		// 補正係数をEEPROMに保存する
		static void SetCorrectionFactors(SensorType type, const CorrectionFactors &cFactors);
						
		// 風速特性係数をEEPROMに保存する
		static void SetVCCoefficients(uint8_t index, const VelocityCharacteristicCoefficients &vcCoefficients);
		
		//EEPROMを読み込む
		static void LoadEEPROM();
	private:
		static void initVCCoefficients(uint8_t index);
		static void initCorrectionFactors(SensorType type);
		static void loadNetworkSettings(void);
		static void loadCorrectionFactors(SensorType type);
		static void loadVCCoefficients(uint8_t index);
};

#endif /* EEPROM_MANAGER_H_ */