/**
 * @file main.h
 * @brief AVR128DB32を使用した計測データ収集・送信プログラム
 * @author E.Togashi
 * @date 2025/6/20
 */

#include "EepromManager.h"

//センサーデータをまとめる構造体
struct SensorData {
	uint32_t snum = 0;
	float temp = 0.0f;
	float humi = 0.0f;
};

//温湿度アキュムレータ（合計値）構造体
struct TH_Accumulator {
	double temp_sum = 0.0;
	double humi_sum = 0.0;
};

//風速アキュムレータ（合計値）構造体
struct VEL_Accumulator {
	double vel_sum = 0.0;
	double vol_sum = 0.0;
};

//ポートとピン番号の構造体
struct PortPin {
	PORT_t* const port; // ポートへのポインタ
	const uint8_t pin_bm;   // ピンのビットマスク
};

static void initialize_port(void);

static void initialize_timer(void);

static float readVelVoltage(uint8_t index);

static void initW5500(void);

static bool initMqttClient(void);

static void runHttpServerMode();

static void runMqttClientMode();

/**
 * @brief 各センサーからデータを1回分読み取り、積算データに加える
 * @param accA Aタイプのセンサーの積算データ配列
 * @param accB Bタイプのセンサーの積算データ配列
 * @param accC Cタイプのセンサーの積算データ配列
 * @param accV 風速センサの積算データ配列
 */
static void collectSensorData(TH_Accumulator accA[], TH_Accumulator accB[], TH_Accumulator accC[], VEL_Accumulator accV[], uint16_t sample_count);

/**
 * @brief 積算データから平均値を計算し、送信データ構造体に格納する
 * @param dataA Aタイプのセンサーの送信データ配列
 * @param dataB Bタイプのセンサーの送信データ配列
 * @param dataC Cタイプのセンサーの送信データ配列
 * @param avg_vel_V 平均風速電圧の送信データ配列
 * @param avg_vel 平均風速の送信データ配列
 * @param accA Aタイプのセンサーの積算データ配列 (入力)
 * @param accB Bタイプのセンサーの積算データ配列 (入力)
 * @param accC Cタイプのセンサーの積算データ配列 (入力)
 * @param accV 風速センサの積算データ配列(入力)
 * @param sample_count 計測回数 (入力)
 */
static void calculateAverages(SensorData dataA[], SensorData dataB[], SensorData dataC[], float avg_vel_V[], float avg_vel[], const TH_Accumulator accA[], const TH_Accumulator accB[], const TH_Accumulator accC[], VEL_Accumulator accV[], uint16_t sample_count);

/**
 * @brief 平均化されたセンサーデータをMQTTで送信する
 * @param dataA Aタイプのセンサーの送信データ配列
 * @param dataB Bタイプのセンサーの送信データ配列
 * @param dataC Cタイプのセンサーの送信データ配列
 * @param avg_vel_V 平均風速電圧の送信データ配列
 * @param avg_vel 平均風速の送信データ配列
 */
static void publishData(const SensorData dataA[], const SensorData dataB[], const SensorData dataC[], float avg_vel_V[], float avg_vel[], uint16_t sample_count);

/**
* @fn
* SDAラインを有効にする
* @param (number) センサ番号
*/
static void enableSDA(uint8_t number);

/**
* @fn
* SDAラインを無効にする
* @param (number) センサ番号
*/
static void disableSDA(uint8_t number);

/**
* @fn
* 有効なSDAラインを変更する
* @param (number) センサ番号
*/
static void changeSDA(uint8_t number);

static bool initSensors(uint8_t number);

//以下はW5500コールバック関数**************************
static void spi_cris_enter(void);

static void spi_cris_exit(void);

static void spi_cs_select(void);

static void spi_cs_deselect(void);

static uint8_t spi_read_byte(void);

static void spi_write_byte(uint8_t data);

static void spi_read_burst(uint8_t* pBuf, uint16_t len);

static void spi_write_burst(uint8_t* pBuf, uint16_t len);

//W5500とのSPI通信テスト用メソッド
static void test_spi_communication(void);

//以下はinline関数************************************

inline static void turnOnGreenAndRedLED(void);

inline static void turnOffGreenAndRedLED(void);

inline static void blinkGreenAndRedLED(int iterNum);

inline static void turnOnGreenLED(void);

inline static void turnOffGreenLED(void);

inline static void toggleGreenLED(void);

inline static void blinkGreenLED(int iterNum);

inline static void turnOnRedLED(void);

inline static void turnOffRedLED(void);

inline static void toggleRedLED(void);

inline static void blinkRedLED(int iterNum);

inline static float max(float x, float y);

inline static float min(float x, float y);

