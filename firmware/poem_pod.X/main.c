#include "mcc_generated_files/system/system.h"
#include "mcc_generated_files/timer/delay.h"
#include "mcc_generated_files/system/pins.h"

#include "main.h"
#include "hal_io.h"
#include "eeprom_manager.h"

//https://github.com/Wiznet/ioLibrary_Driver の公式ライブラリを使用
#include "W5500/w5500.h" //W5500によるEthernet通信
#include "W5500/socket.h"
#include "W5500/MQTTClient.h" //MQTT通信
#include "W5500/httpServer.h" //HTTPサーバー

#define TCP_SOCKET (0)
#define MQTT_PORT (1883) //MQTT通信用ポート
#define MQTT_HTTP_BUF_SIZE (2048)

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

//初期化中フラグ
volatile bool isInitMode = true;


// 1msecごとのコールバック関数
void msecHandler(void)
{
    MilliTimer_Handler();
}

int main(void)
{
    SYSTEM_Initialize();
    
    // 電力安定化まで0.5s待つ
	DELAY_milliseconds(500);
    
    // EEPROM読み込みまたは初期化
    EM_loadEEPROM();
    
    // イベントハンドラ登録
    TCB0_OverflowCallbackRegister(msecHandler); // 1msecタイマ
    
    //WIZnetドライバにコールバック関数を登録
	reg_wizchip_cris_cbfunc(spi_cris_enter, spi_cris_exit);
	reg_wizchip_cs_cbfunc(spi_cs_select, spi_cs_deselect);
	reg_wizchip_spi_cbfunc(spi_read_byte, spi_write_byte);
	if (WIZCHIP.IF.SPI._read_burst && WIZCHIP.IF.SPI._write_burst) { // 関数ポインタが定義されていれば
		reg_wizchip_spiburst_cbfunc(spi_read_burst, spi_write_burst);
	}
    
    //割り込み再開
	sei();

    //Resetスイッチ押し込み中の場合には初期設定モードへ
	isInitMode = IO_RESET_GetValue();
	
	//モード切り分け
	if(isInitMode) runMqttClientMode();
	else runHttpServerMode(); 
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
	/*//計測データを保持
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
	}*/
}

// <editor-fold defaultstate="collapsed" desc="W5500用コールバック関数">

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

// </editor-fold>