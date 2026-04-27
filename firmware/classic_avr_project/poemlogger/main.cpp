/**
 * @file main.cpp
 * @brief AVR(AVRxxDB32)とPoEを使用した計測データ収集・送信プログラム
 * @author E.Togashi
 * @date 2025/5/25
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

#include <avr/xmega.h>

#ifdef __cplusplus
extern "C"{
#endif
	#include <avr/cpufunc.h>
#ifdef __cplusplus
} // extern "C"
#endif

#include <util/delay.h>
#include <avr/wdt.h>

#include "main.h"
#include "EepromManager.h"	//EEPROM処理
#include "i2c/I2cDriver.h"  //I2C通信
#include "i2c/Sht4x.h"		//温湿度センサ
#include "Utilities.h"		//CRC8チェックなど

//https://github.com/Wiznet/ioLibrary_Driver の公式ライブラリを使用
#include "W5500/w5500.h" //W5500によるEthernet通信
#include "W5500/socket.h"
#include "W5500/MQTTClient.h" //MQTT通信
#include "W5500/httpServer.h" //HTTPサーバー

#define TCP_SOCKET 0
#define MQTT_HTTP_BUF_SIZE 2048
#define NUM_TH_SENSORS 5
#define NUM_VEL_SENSORS 4
#define REF_VOLTAGE 2.0
#define AD_RESOLUTION 65536

//MQTT出力リクエスト
volatile bool g_publish_request = false;

//定数宣言***********************************************************
//トップページHTML
const char index_page_content[] =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<title>PoEM-Logger Configuration Menu</title>"
"<style>"
"body{font-family:sans-serif; width:450px; margin:40px auto; background-color:#f4f4f4; color:#333;}"
"h1{text-align:center; color:#555;}"
"ul{list-style:none; padding:0;}"
"li a{display:block; padding:15px; margin:10px 0; background-color:#fff; text-decoration:none; color:#007bff; border-radius:5px; border:1px solid #ddd; text-align:center; transition: background-color 0.2s;}"
"li a:hover{background-color:#e9ecef;}"
"</style>"
"</head>"
"<body>"
"<h1>Configuration Menu</h1>"
"<ul>"
"<li><a href=\"/net.cgi\">Network Settings</a></li>"
"<li><a href=\"/correct_a.cgi\">Correction Factors A</a></li>"
"<li><a href=\"/correct_b.cgi\">Correction Factors B</a></li>"
"<li><a href=\"/correct_c.cgi\">Correction Factors C</a></li>"
"<li><a href=\"/vel.cgi\">Velocity Coefficients</a></li>"
"</ul>"
"</body>"
"</html>";

//MQTT通信用ポート
const uint16_t MQTT_PORT = 1883;

//SDA制御用ピン
const PortPin SDA_PINS[NUM_TH_SENSORS] = {
	{&PORTF, PIN4_bm}, // センサー1
	{&PORTF, PIN5_bm}, // センサー2
	{&PORTA, PIN0_bm}, // センサー3
	{&PORTA, PIN1_bm}, // センサー4
	{&PORTA, PIN2_bm}  // センサー5
};

//リセット処理用
volatile static unsigned int resetTime = 0;

//初期化中フラグ
volatile bool isInitMode = true;

//現在有効なSDA番号
static short int enabledSDA = 1;

//MQTT通信用のクライアント
static MQTTClient mqtt_client;

/**
* @fn
* ウォッチドッグタイマーを利用してMCUをリセットする
*/
static void mcu_reset(void) {
	wdt_enable(WDTO_15MS); // ウォッチドッグを有効化し、最短時間(15ms)でタイムアウト設定
	while(1) {}            // 無限ループに入り、WDTによるリセットを待つ
}

int main(void)
{	
	//電力安定化まで0.5s待つ
	_delay_ms(500);
	
	//EEPROM読み込みまたは初期化
	EepromManager::LoadEEPROM();

	//入出力ポートを初期化
	initialize_port();
		
	//WIZnetドライバにコールバック関数を登録
	reg_wizchip_cris_cbfunc(spi_cris_enter, spi_cris_exit);
	reg_wizchip_cs_cbfunc(spi_cs_select, spi_cs_deselect);
	reg_wizchip_spi_cbfunc(spi_read_byte, spi_write_byte);
	if (WIZCHIP.IF.SPI._read_burst && WIZCHIP.IF.SPI._write_burst) { // 関数ポインタが定義されていれば
		reg_wizchip_spiburst_cbfunc(spi_read_burst, spi_write_burst);
	}

	//一旦、すべての割り込み禁止
	cli();
		
	//AD変換の設定
	ADC0.CTRLA |= (ADC_ENABLE_bm | ADC_RESSEL_10BIT_gc); //ADC有効化, 10bit分解
	ADC0.CTRLB |= ADC_SAMPNUM_ACC64_gc; //64回平均化, 16, 32, 64から設定できる
	ADC0.CTRLC |= ADC_PRESC_DIV128_gc; //128分周で計測（大きい方が精度は高く、時間はかかる模様）
	VREF.ADC0REF = VREF_REFSEL_VREFA_gc; //基準電圧をVREFA(2.0V)に設定
	
	//温湿度センサ初期化
	I2cDriver::initialize(); //I2C通信
	for(int i=0;i<NUM_TH_SENSORS;i++) disableSDA(i + 1); //一旦すべてのセンサを無効にする
	for(int i=0;i<NUM_TH_SENSORS;i++) initSensors(i + 1); //1つずつ初期化
	
	//タイマ初期化
	initialize_timer();

	//W5500を用意
	initW5500();
	
	//初期設定が終わったら少し待機
	_delay_ms(500);

	//割り込み再開
	sei();
	
	//W5500動作試験(debug用)	
	//test_spi_communication();
	
	//Resetスイッチ押し込み中の場合には初期設定モードへ
	isInitMode = !(PORTD.IN & PIN6_bm);
	
	//モード切り分け
	if(isInitMode) runHttpServerMode();
	else runMqttClientMode();
}

static void runHttpServerMode(){
	uint8_t g_http_socklist[] = {1}; // HTTPソケット
		
	//HTTPサーバー初期化
	uint8_t http_tx_buff[MQTT_HTTP_BUF_SIZE];
	uint8_t http_rx_buff[MQTT_HTTP_BUF_SIZE];
	httpServer_init(http_tx_buff, http_rx_buff, 1, g_http_socklist);
	// httpServerにリセット関数を登録する
	//reg_httpServer_cbfunc(mcu_reset, NULL); // 第2引数はWDTリセット用だが今回は未使用
	//ページを登録
	reg_httpServer_webContent((uint8_t *)"index.html", (uint8_t *)index_page_content);
	//初期化モードの場合は赤LED点灯
	turnOnRedLED();
	
	while(true) httpServer_run(g_http_socklist[0]);
}

static void runMqttClientMode(){
	//計測データを保持
	SensorData sensor_data_A[NUM_TH_SENSORS], sensor_data_B[NUM_TH_SENSORS], sensor_data_C[NUM_TH_SENSORS];
	float avg_vel_V[NUM_VEL_SENSORS], avg_vel[NUM_VEL_SENSORS];
	TH_Accumulator accumulator_A[NUM_TH_SENSORS], accumulator_B[NUM_TH_SENSORS], accumulator_C[NUM_TH_SENSORS];
	VEL_Accumulator accumulator_V[NUM_VEL_SENSORS];
	//計測回数を保持
	uint16_t sample_count = 0;
	
	// 初回接続試行
	while(!initMqttClient()) {
		blinkRedLED(2); // 接続失敗時にLED点滅
		_delay_ms(2000); // 2秒待機
	}
	
	while(true) {
		// MQTT接続確認
		if (!mqtt_client.isconnected || getSn_SR(TCP_SOCKET) != SOCK_ESTABLISHED) {
			// 既存の接続を閉じる
			disconnect(TCP_SOCKET);

			// 再接続を試みる
			while(!initMqttClient()) {
				blinkRedLED(3); // 再接続失敗時にLED点滅
				_delay_ms(3000); // 3秒待機してリトライ
			}
		}
		
		// MQTT接続の維持
		if(mqtt_client.isconnected) MQTTYield(&mqtt_client, 10);

		// MQTT送信要求あり
		if(g_publish_request) {
			g_publish_request = false;

			if (0 < sample_count) {
				//計測データを平均化
				calculateAverages(
					sensor_data_A,sensor_data_B, sensor_data_C, avg_vel_V, avg_vel,
					accumulator_A, accumulator_B, accumulator_C, accumulator_V, sample_count);
					
				//MQTTで送信
				publishData(sensor_data_A, sensor_data_B, sensor_data_C, avg_vel_V, avg_vel, sample_count);
				
				//送信成功をLEDで通知
				blinkGreenLED(1);
				
				//データを初期化
				memset(accumulator_A, 0, sizeof(accumulator_A));
				memset(accumulator_B, 0, sizeof(accumulator_B));
				memset(accumulator_C, 0, sizeof(accumulator_C));
				memset(accumulator_V, 0, sizeof(accumulator_V));
				sample_count = 0;
			}
		}

		//各センサーからデータを1回分読み取り、積算データに加える
		collectSensorData(accumulator_A, accumulator_B, accumulator_C, accumulator_V, sample_count);
		
		//計測回数を保存
		sample_count++;
	}
}

/**
 * @brief 各センサーからデータを1回分読み取り、積算データに加える
 */
static void collectSensorData(TH_Accumulator accA[], TH_Accumulator accB[], TH_Accumulator accC[], VEL_Accumulator accV[], uint16_t sample_count)
{
	// 温湿度データ計測
	for(int i = 0; i < NUM_TH_SENSORS; i++) {
		changeSDA(i + 1); // 有効なSDAラインを変更
		_delay_ms(10);    // 安定化待ち

		float temp_a, humi_a, temp_b, humi_b, temp_c, humi_c;
		
        // SHT4x-ADの読み取りと補正
		bool sucA = Sht4x::readValue(&temp_a, &humi_a, Sht4x::SHT4_AD); //
		if(sucA){ // 読み取り成功時
			const CorrectionFactors& cfa = EepromManager::cFactors[EepromManager::A_type]; //
			temp_a = temp_a * cfa.coeffs[i].temp_a + cfa.coeffs[i].temp_b; //
			humi_a = humi_a * cfa.coeffs[i].humi_a + cfa.coeffs[i].humi_b; //
			humi_a = max(0.0f, min(100.0f, humi_a)); //
			temp_a = max(-50.0f, min(150.0f, temp_a)); //
		}
		else { // 読み取り失敗時、直前の平均値で代用
			temp_a = sample_count == 0 ? 0 : (accA[i].temp_sum / sample_count); //
			humi_a = sample_count == 0 ? 0 : (accA[i].humi_sum / sample_count); //
		}

		// SHT4x-BDの読み取りと補正 (Aと同様の処理)
		bool sucB = Sht4x::readValue(&temp_b, &humi_b, Sht4x::SHT4_BD); //
		if(sucB){
			const CorrectionFactors& cfb = EepromManager::cFactors[EepromManager::B_type]; //
			temp_b = temp_b * cfb.coeffs[i].temp_a + cfb.coeffs[i].temp_b; //
			humi_b = humi_b * cfb.coeffs[i].humi_a + cfb.coeffs[i].humi_b; //
			humi_b = max(0.0f, min(100.0f, humi_b)); //
			temp_b = max(-50.0f, min(150.0f, temp_b)); //
		}
		else {
			temp_b = sample_count == 0 ? 0 : (accB[i].temp_sum / sample_count); //
			humi_b = sample_count == 0 ? 0 : (accB[i].humi_sum / sample_count); //
		}

		// SHT4x-CDの読み取りと補正 (Aと同様の処理)
		bool sucC = Sht4x::readValue(&temp_c, &humi_c, Sht4x::SHT4_CD); //
		if(sucC){
			const CorrectionFactors& cfc = EepromManager::cFactors[EepromManager::C_type]; //
			temp_c = temp_c * cfc.coeffs[i].temp_a + cfc.coeffs[i].temp_b; //
			humi_c = humi_c * cfc.coeffs[i].humi_a + cfc.coeffs[i].humi_b; //
			humi_c = max(0.0f, min(100.0f, humi_c)); //
			temp_c = max(-50.0f, min(150.0f, temp_c)); //
		}
		else {
			temp_c = sample_count == 0 ? 0 : (accC[i].temp_sum / sample_count); //
			humi_c = sample_count == 0 ? 0 : (accC[i].humi_sum / sample_count); //
		}

		// 計算した値を積算
		accA[i].temp_sum += temp_a; accA[i].humi_sum += humi_a; //
		accB[i].temp_sum += temp_b; accB[i].humi_sum += humi_b; //
		accC[i].temp_sum += temp_c; accC[i].humi_sum += humi_c; //
	}

	// 風速データ計測と積算
	for(int i=0;i<NUM_VEL_SENSORS;i++){
		float velV = readVelVoltage(i + 1);
		float bff = max(0, velV / EepromManager::vcCoefficients[i].vol0 - 1.0);
		float vel = EepromManager::vcCoefficients[i].ccB * pow(bff, EepromManager::vcCoefficients[i].ccA);
		accV[i].vol_sum += velV;
		accV[i].vel_sum += vel;
	}	
}

/**
 * @brief 積算データから平均値を計算し、送信データ構造体に格納する
 */
static void calculateAverages(SensorData dataA[], SensorData dataB[], SensorData dataC[], float avg_vel_V[], float avg_vel[],
	const TH_Accumulator accA[], const TH_Accumulator accB[], const TH_Accumulator accC[], VEL_Accumulator accV[], uint16_t sample_count)
{
	// sample_countが0の場合は除算エラーを防ぐために関数を抜ける
	if (sample_count == 0) return;

	// 温湿度の平均値を計算
	for(int i = 0; i < NUM_TH_SENSORS; i++) {
		dataA[i].temp = accA[i].temp_sum / sample_count; //
		dataA[i].humi = accA[i].humi_sum / sample_count; //
		dataB[i].temp = accB[i].temp_sum / sample_count; //
		dataB[i].humi = accB[i].humi_sum / sample_count; //
		dataC[i].temp = accC[i].temp_sum / sample_count; //
		dataC[i].humi = accC[i].humi_sum / sample_count; //
	}

	// 風速の平均値を計算
	for(int i = 0; i < NUM_VEL_SENSORS; i++) {
		avg_vel_V[i] = accV[i].vol_sum / sample_count;
		avg_vel[i] = accV[i].vel_sum / sample_count;
	}

	// 送信用シリアル番号を取得（抜き差しに備えて毎回再読み込み）
	for (int i = 0; i < NUM_TH_SENSORS; ++i) {
		changeSDA(i + 1); //
		_delay_ms(10);    //
		Sht4x::readSerial(&dataA[i].snum, Sht4x::SHT4_AD); //
		Sht4x::readSerial(&dataB[i].snum, Sht4x::SHT4_BD); //
		Sht4x::readSerial(&dataC[i].snum, Sht4x::SHT4_CD); //
	}
}

/**
 * @brief 平均化されたセンサーデータをMQTTで送信する
 */
static void publishData(const SensorData dataA[], const SensorData dataB[], const SensorData dataC[], float avg_vel_V[], float avg_vel[], uint16_t sample_count)
{
	static unsigned char mqtt_payload[MQTT_HTTP_BUF_SIZE]; //
	MQTTMessage message; //
	message.qos = QOS0; //
	message.retained = 0; //
	message.payload = mqtt_payload; //

	char* p = (char*)mqtt_payload;
	char* const end = p + sizeof(mqtt_payload);

	// JSON文字列を生成
	p += snprintf(p, end - p, "{");
		
	for (int i = 0; i < NUM_TH_SENSORS; ++i) {
		p += snprintf(p, end - p, "%s\"%d\":{", (i == 0 ? "" : ","), i + 1);
			
		p += snprintf(p, end - p, "\"A\":{\"sn\":%lu,\"t\":%.0f,\"h\":%.0f}", dataA[i].snum, dataA[i].temp * 100.0, dataA[i].humi * 100.0); //
		p += snprintf(p, end - p, ",\"B\":{\"sn\":%lu,\"t\":%.0f,\"h\":%.0f}", dataB[i].snum, dataB[i].temp * 100.0, dataB[i].humi * 100.0); //
		p += snprintf(p, end - p, ",\"C\":{\"sn\":%lu,\"t\":%.0f,\"h\":%.0f}", dataC[i].snum, dataC[i].temp * 100.0, dataC[i].humi * 100.0); //
			
		p += snprintf(p, end - p, "}");
	}
	for (int i = 0; i < NUM_VEL_SENSORS; ++i) {
		p += snprintf(p, end - p, ",\"vV%d\":%.0f,\"v%d\":%.0f", i + 1, avg_vel_V[i] * 10000.0, i + 1, avg_vel[i] * 100.0);
	}
	p += snprintf(p, end - p, ",\"s_cnt\":%u}", sample_count);

	message.payloadlen = strlen((char*)mqtt_payload); //

	// トピック名を生成
	char full_topic[32];
	snprintf(full_topic, sizeof(full_topic), "poem/%s", EepromManager::net_settings.topic_suffix); //

	// MQTTで送信
	MQTTPublish(&mqtt_client, full_topic, &message);
}

//ポート初期化
static void initialize_port(void)
{
	//PA0
	PORTA.DIRSET = PIN0_bm; //出力（SDA3）
	
	//PA1
	PORTA.DIRSET = PIN1_bm; //出力（SDA4）
	
	//PA2
	PORTA.DIRSET = PIN2_bm; //出力（SDA5）
	
	//PA3
	PORTA.PIN3CTRL = PORT_ISC_INPUT_DISABLE_gc; // 未使用
	
	//PA4,5,6,7はW5500とのSPI通信（initW5500()で初期化）
	//***
	
	//PC0
	PORTC.DIRSET = PIN0_bm; //出力（緑LED）

	//PC1
	PORTC.DIRSET = PIN1_bm; //出力（赤LED）

	//PC2
	PORTC.PIN2CTRL = PORT_ISC_INPUT_DISABLE_gc; // 未使用
	
	//PC3
	PORTC.PIN3CTRL = PORT_ISC_INPUT_DISABLE_gc; // 未使用
	
	//PD1
	PORTD.DIRCLR = PIN1_bm; //入力（風速:AD変換1）
	
	//PD2
	PORTD.DIRCLR = PIN2_bm; //入力（風速:AD変換2）
	
	//PD3
	PORTD.PIN3CTRL = PORT_ISC_INPUT_DISABLE_gc; // 未使用
	
	//PD4
	PORTD.DIRCLR = PIN4_bm; //入力（風速:AD変換3）
	
	//PD5
	PORTD.DIRCLR = PIN5_bm; //入力（風速:AD変換4）
	
	//PD6
	PORTD.DIRCLR = PIN6_bm; //入力（初期化スイッチ）
	PORTD.OUTSET = PIN6_bm; //プルアップ
	
	//PD7
	PORTD.DIRCLR = PIN7_bm; //入力（アナログ基準電圧）
	
	//PF0,1はクリスタル
	//***
	
	//PF2,3はI2C通信
	//***
	
	//PF4
	PORTF.DIRSET = PIN4_bm; //出力（SDA1）
	
	//PF5
	PORTF.DIRSET = PIN5_bm; //出力（SDA2）
	
	//PF6はリセット
	//***
}

//タイマ初期化
static void initialize_timer( void )
{	
	//W5500通信用の0.001secタイマ
	TCB0.CTRLB = TCB_CNTMODE_INT_gc; // Periodic Interruptモード
	TCB0.CCMP = (F_CPU / 1000UL) - 1; //0.001sec, 4MHz, 前置分周器無し
	TCB0.INTCTRL = TCB_CAPT_bm;      // 割り込みを有効化	
	TCB0.CTRLA = TCB_CLKSEL_DIV1_gc | TCB_ENABLE_bm; //分周器無し、タイマー有効化
			
	//外部クリスタルによる1secタイマ*******************
	//**外部クリスタルの有効化処理**
	//発振器禁止
	_PROTECTED_WRITE(CLKCTRL.XOSC32KCTRLA, ~CLKCTRL_ENABLE_bm);
	while(CLKCTRL.MCLKSTATUS & CLKCTRL_XOSC32KS_bm); //XOSC32KSが0になるまで待機

	//XTAL1とXTAL2に接続された外部クリスタルを使用
	_PROTECTED_WRITE(CLKCTRL.XOSC32KCTRLA, ~CLKCTRL_SEL_bm);
	
	//発振器許可
	_PROTECTED_WRITE(CLKCTRL.XOSC32KCTRLA, CLKCTRL_ENABLE_bm);

	while (RTC.STATUS > 0); //全レジスタが同期されるまで待機
	//**有効化処理ここまで*********

	RTC.CLKSEL = RTC_CLKSEL_XOSC32K_gc;	  //32.768kHz外部クリスタル用発振器 (XOSC32K)を選択
	RTC.DBGCTRL |= RTC_DBGRUN_bm;         //デバッグで走行を許可
	RTC.PITINTCTRL = RTC_PI_bm;           //定期割込を有効にする
	RTC.PITCTRLA = RTC_PERIOD_CYC32768_gc //RTC周期は32768
				| RTC_PITEN_bm;           //定期割込用タイマを有効にする
	
	//POWER DOWN時にもタイマ割込を有効にする
	SLPCTRL.CTRLA |= SLPCTRL_SMODE_PDOWN_gc; 
	SLPCTRL.CTRLA |= SLPCTRL_SEN_bm;
}

// W5500用0.001secタイマ
ISR(TCB0_INT_vect)
{
	MilliTimer_Handler(); // mqtt_interface.c で定義されているグローバル変数をインクリメント
	TCB0.INTFLAGS = TCB_CAPT_bm; // 割り込みフラグをクリア
}

//ロギング用の1秒毎の処理
ISR(RTC_PIT_vect)
{
	//割り込み要求フラグ解除
	RTC.PITINTFLAGS = RTC_PI_bm;
	
	//ネットワーク設定の初期化中
	if(isInitMode) 
	{
		//リセットスイッチ確認
		if(!(PORTD.IN & PIN6_bm)) resetTime++;
		else resetTime = 0;		
		//5sec以上押し続けた場合はネットワーク設定初期化
		if(5 <= resetTime) {
			//初期化処理
			EepromManager::InitializeNetworkSettings();
			resetTime = 0;
			blinkRedLED(4);
			mcu_reset(); 
		}
		
		return;
	}
	
	//MQTT送信要求フラグOn
	g_publish_request = true;
}

//微風速の電圧を読み取る
static float readVelVoltage(uint8_t index)
{
	switch(index){
		case 1:	//AI1を計測
			ADC0.MUXPOS = ADC_MUXPOS_AIN1_gc;
			break;
		case 2: //AI2を計測
			ADC0.MUXPOS = ADC_MUXPOS_AIN2_gc;
			break;
		case 3: //AI4を計測			
			ADC0.MUXPOS = ADC_MUXPOS_AIN4_gc;
			break;		
		case 4: //AI5を計測
			ADC0.MUXPOS = ADC_MUXPOS_AIN5_gc;
			break;
		default://AI1を計測
			ADC0.MUXPOS = ADC_MUXPOS_AIN1_gc;
			return 0;		
	}
	_delay_ms(5);
	ADC0.COMMAND = ADC_STCONV_bm; //変換開始
	while (!(ADC0.INTFLAGS & ADC_RESRDY_bm)) ; //変換終了待ち
	return REF_VOLTAGE * (float)ADC0.RES / AD_RESOLUTION; //1024*64 (10bit,64回平均)
}

static void enableSDA(uint8_t number){
	if (number < 1 || number > NUM_TH_SENSORS) return; // 範囲外の番号は無視
	const PortPin& sda = SDA_PINS[number - 1];

	// 入力にしてプルダウン
	sda.port->DIRCLR = sda.pin_bm;
	sda.port->OUTCLR = sda.pin_bm;
}

static void disableSDA(uint8_t number){
	if (number < 1 || number > NUM_TH_SENSORS) return; // 範囲外の番号は無視
	const PortPin& sda = SDA_PINS[number - 1];

	// 出力にしてHIGH
	sda.port->DIRSET = sda.pin_bm;
	sda.port->OUTSET = sda.pin_bm;
}

static void changeSDA(uint8_t number){
	disableSDA(enabledSDA);
	enabledSDA = number;
	enableSDA(number);
}

static bool initSensors(uint8_t number)
{
	//有効なSDAラインを変更
	changeSDA(number);
	
	//電源Onの後、少し待つ
	_delay_ms(10);
	
	return (
		Sht4x::initialize(Sht4x::SHT4_AD) &&
	    Sht4x::initialize(Sht4x::SHT4_BD) &&
	    Sht4x::initialize(Sht4x::SHT4_CD));
}

//W5500を初期化する
static void initW5500(void){
	//ピンの初期設定
	PORTA.DIR |= PIN4_bm;  //SPI通信（MOSI）
	PORTA.DIR &= ~PIN5_bm; //SPI通信（MISO）
	PORTA.DIR |= PIN6_bm;  //SPI通信（SCK）
	PORTA.DIR |= PIN7_bm;  //SPI通信（CS）
	PORTA.OUTSET = PIN7_bm; //CSをHiに設定
	PORTA.PIN5CTRL = PORT_PULLUPEN_bm; //MISOの内部プルアップを有効化
	
	//SPI通信の初期設定
	SPI0.CTRLA = 0; // 一旦、SPIを無効化
	// マスターモード、クロック2x無効(デフォルト)、16分周を設定
	SPI0.CTRLA = SPI_MASTER_bm | SPI_PRESC_DIV16_gc;
	SPI0.CTRLB = SPI_MODE_0_gc;  // CPOL = 0, CPHA = 0 //W5500の仕様
	SPI0.CTRLA |= SPI_ENABLE_bm; // SPIを有効化
	
	//バッファサイズ設定と初期化
	uint8_t txsize[8] = {2,2,2,2,2,2,2,2};
	uint8_t rxsize[8] = {2,2,2,2,2,2,2,2};
	wizchip_init(txsize, rxsize);
	
	//通信の設定
	wiz_NetInfo netinfo;
	
	//MACアドレス
	netinfo.mac[0] = 0x00; // 上3桁はWIZnetのOUI
	netinfo.mac[1] = 0x08;
	netinfo.mac[2] = 0xDC;
	netinfo.mac[3] = SIGROW.SERNUM7; //下3桁はAVRマイコンのUIDで代替（本来はMACアドレスを取得すべき）
	netinfo.mac[4] = SIGROW.SERNUM8;
	netinfo.mac[5] = SIGROW.SERNUM9;
	
	//IPアドレス、サブネットマスク、デフォルトゲートウェイ、DNSを設定
	memcpy(netinfo.ip, EepromManager::net_settings.ip, 4);
	memcpy(netinfo.sn, EepromManager::net_settings.sn, 4);
	memcpy(netinfo.gw, EepromManager::net_settings.gw, 4);
	memcpy(netinfo.dns, EepromManager::net_settings.dns, 4);
	
	//固定IPを使用
	netinfo.dhcp = NETINFO_STATIC;

	ctlnetwork(CN_SET_NETINFO, (void*)&netinfo);
}

static bool initMqttClient(void){
	Network mqtt_net;
	uint8_t broker_ip[4];
	memcpy(broker_ip, EepromManager::net_settings.bip, 4);
			
	// ネットワークインターフェースの初期化
	NewNetwork(&mqtt_net, TCP_SOCKET);
	//接続試行
	if(ConnectNetwork(&mqtt_net, broker_ip, MQTT_PORT) != SOCK_OK) {
		return false;
	}

	//MQTTクライアントの初期化
	uint8_t mq_tx_buff[MQTT_HTTP_BUF_SIZE];
	uint8_t mq_rx_buff[MQTT_HTTP_BUF_SIZE];
	MQTTClientInit(&mqtt_client, &mqtt_net, 3000, mq_tx_buff, sizeof(mq_tx_buff), mq_rx_buff, sizeof(mq_rx_buff));
	
	//マイコンIDをもとにMQTTクライアント用ID（MACアドレスで代替）を生成
	char client_id_buf[40];
	snprintf(client_id_buf, sizeof(client_id_buf), "%02X-%02X-%02X-%02X-%02X-%02X",
	0x00, 0x08, 0xDC,  //上3桁はWIZnetのOUI
	SIGROW.SERNUM7, SIGROW.SERNUM8, SIGROW.SERNUM9); //下3桁はAVRマイコンのUIDで代替（本来はMACアドレスを取得すべき）
	
	//MQTT接続オプションの設定
	MQTTPacket_connectData mqtt_data = MQTTPacket_connectData_initializer;
	mqtt_data.MQTTVersion = 3;
	mqtt_data.clientID.cstring = client_id_buf;
	mqtt_data.keepAliveInterval = 60;
	mqtt_data.cleansession = 1;

	// MQTTブローカーへ接続
	if (MQTTConnect(&mqtt_client, &mqtt_data) != SUCCESSS) {
		return false;
	}
	
	return true;
}

//以下はW5500コールバック関数**************************
// クリティカルセクション入
void spi_cris_enter(void) {
	cli(); // 全割り込み禁止
}

// クリティカルセクション出
void spi_cris_exit(void) {
	sei(); // 全割り込み許可
}

// チップセレクト (CS) 有効化 (Lowにする)
void spi_cs_select(void) {
	PORTA.OUTCLR = PIN7_bm; // PA7をLowに
}

// チップセレクト (CS) 無効化 (Highにする)
void spi_cs_deselect(void) {
	PORTA.OUTSET = PIN7_bm; // PA7をHighに
}

// 1バイトSPI読み出し
uint8_t spi_read_byte(void) {
	SPI0.DATA = 0xFF; // ダミーデータを送信して読み出しを開始
	while (!(SPI0.INTFLAGS & SPI_IF_bm)); // 送受信完了待ち (SPIFフラグが立つまで)
	return SPI0.DATA; // 受信データを返す
}

// 1バイトSPI書き込み
void spi_write_byte(uint8_t data) {
	SPI0.DATA = data; // データを送信
	while (!(SPI0.INTFLAGS & SPI_IF_bm)); // 送信完了待ち (SPIFフラグが立つまで)
	// W5500では書き込み時のMISOは通常無視できるが、フラグクリアのために読み出す
	(void)SPI0.DATA;
}

// バーストSPI読み出し
void spi_read_burst(uint8_t* pBuf, uint16_t len) {
	for (uint16_t i = 0; i < len; i++) {
		pBuf[i] = spi_read_byte();
	}
}

// バーストSPI書き込み
void spi_write_burst(uint8_t* pBuf, uint16_t len) {
	for (uint16_t i = 0; i < len; i++) {
		spi_write_byte(pBuf[i]);
	}
}

void test_spi_communication(void)
{
	uint8_t version = getVERSIONR();

	//結果の確認
	if (version == 0x04) {
		// 成功：緑LEDを5回点滅させる
		for (int i = 0; i < 5; i++) {
			turnOnGreenLED();
			_delay_ms(100);
			turnOffGreenLED();
			_delay_ms(100);
		}
	} else {
		// 失敗：赤LEDを連続で高速点滅させる (ハングアップ)
		while (1) {
			turnOnRedLED();
			_delay_ms(50);
			turnOffRedLED();
			_delay_ms(50);
		}
	}
}

//以下はinline関数************************************

inline static void turnOnGreenAndRedLED(void)
{
	turnOnGreenLED();
	turnOnRedLED();
}

inline static void turnOffGreenAndRedLED(void)
{
	turnOffGreenLED();
	turnOffRedLED();
}

inline static void blinkGreenAndRedLED(int iterNum)
{
	if(iterNum < 1) return;

	//初回
	turnOffGreenAndRedLED(); //一旦必ず消灯して
	//点滅
	for(int i=0;i<iterNum;i++)
	{
		_delay_ms(100);
		turnOnGreenAndRedLED(); //点灯
		_delay_ms(25);
		turnOffGreenAndRedLED(); //消灯
	}
}

inline static void turnOnGreenLED(void)
{
	PORTC.OUTSET = PIN0_bm; //点灯
}

inline static void turnOffGreenLED(void)
{
	PORTC.OUTCLR = PIN0_bm; //消灯
}

inline static void toggleGreenLED(void)
{
	PORTC.OUTTGL = PIN0_bm; //反転
}

inline static void blinkGreenLED(int iterNum)
{
	if(iterNum < 1) return;

	//初回
	turnOffGreenLED(); //一旦必ず消灯して
	//点滅
	for(int i=0;i<iterNum;i++)
	{
		_delay_ms(100);
		turnOnGreenLED(); //点灯
		_delay_ms(25);
		turnOffGreenLED(); //消灯
	}
}

inline static void turnOnRedLED(void)
{
	PORTC.OUTSET = PIN1_bm; //点灯
}

inline static void turnOffRedLED(void)
{
	PORTC.OUTCLR = PIN1_bm; //消灯
}

inline static void toggleRedLED(void)
{
	PORTC.OUTTGL = PIN1_bm; //反転
}

inline static void blinkRedLED(int iterNum)
{
	if(iterNum < 1) return;

	//初回
	turnOffRedLED(); //一旦必ず消灯して
	//点滅
	for(int i=0;i<iterNum;i++)
	{
		_delay_ms(100);
		turnOnRedLED(); //点灯
		_delay_ms(25);
		turnOffRedLED(); //消灯
	}
}

inline static float max(float x, float y)
{
	return (x > y) ? x : y;
}

inline static float min(float x, float y)
{
	return (x < y) ? x : y;
}