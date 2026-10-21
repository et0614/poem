#include "mcc_generated_files/system/system.h"
#include "mcc_generated_files/system/ccp.h"
#include "mcc_generated_files/timer/delay.h"
#include "mcc_generated_files/timer/rtc.h"
#include "mcc_generated_files/system/pins.h"

#include <avr/wdt.h>
#include <util/atomic.h>
#include <util/delay_basic.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "hal_io.h"
#include "eeprom_manager.h"
#include "node_manager.h"
#include "mqtt_app.h"
#include "i2c_master.h"

//https://github.com/Wiznet/ioLibrary_Driver の公式ライブラリを使用
#include "W5500/w5500.h" //W5500によるEthernet通信
#include "W5500/socket.h"
#include "W5500/MQTTClient.h" //MQTT通信
#include "W5500/httpServer.h" //HTTPサーバー

// 親機のファームウェアの版 (poem/<pod>/pod/info で送る)
#define POD_FW_MAJOR            2
#define POD_FW_MINOR            0
#define POD_FW_PATCH            0

// SCAN ボタンのデバウンス用閾値 (ms)。連続して押下を検知するための最小間隔。
#define SCAN_DEBOUNCE_MS        50

// 完全初期化と判定する Reset スイッチの長押し時間 (ms)。HTTP 設定モード中のみ有効。
#define FACTORY_RESET_HOLD_MS   5000

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
"<li><a href=\"/devices.cgi\">Devices</a></li>"
"</ul>"
"</body>"
"</html>";

//初期化中フラグ。Reset スイッチ押し込み起動なら true → HTTP 設定モード。
volatile bool isInitMode = true;

// 1秒ごとに立つ MQTT 送信要求フラグ。RTC OVF で true に。
volatile bool g_publish_request = false;

// MilliTimer は mqtt_interface.c のグローバル (1ms 毎にインクリメント)
extern unsigned long MilliTimer;


// 1msecごとのコールバック関数
void msecHandler(void)
{
    MilliTimer_Handler();
}

// 1secごとのコールバック (RTC OVF, 32768/32768Hz = 1.0s)
static void secHandler(void)
{
    g_publish_request = true;
}

// MCC の SPI0_Initialize() で設定された値を W5500 用に書き換える。
// MCC Melody の SPI Abstract Driver は GUI で PRESC/CLK2X/BUFEN を設定できないため、
// SYSTEM_Initialize() の直後に明示的に再設定する。
//   - 6 MHz (F_CPU=24MHz, PRESC=DIV4, CLK2X=OFF)
//   - BUFEN/BUFWR 有効 (連続バイト送信を効率化)
//   - SPI Mode 0 (CPOL=0, CPHA=0; W5500 仕様)
//   - SS Disable (CS は PA7 を GPIO で制御)
static void spi0_reconfigure_for_w5500(void)
{
    SPI0.CTRLA &= ~SPI_ENABLE_bm;
    SPI0.CTRLA = (0 << SPI_CLK2X_bp)
               | (0 << SPI_DORD_bp)
               | (1 << SPI_MASTER_bp)
               | SPI_PRESC_DIV4_gc;
    SPI0.CTRLB = (1 << SPI_BUFEN_bp)
               | (1 << SPI_BUFWR_bp)
               | SPI_MODE_0_gc
               | (1 << SPI_SSD_bp);
    SPI0.CTRLA |= SPI_ENABLE_bm;
}

int main(void)
{
    SYSTEM_Initialize();

    // SPI0 を W5500 用に上書き設定
    spi0_reconfigure_for_w5500();

    // 電力安定化まで0.5s待つ
	DELAY_milliseconds(500);

    // EEPROM読み込みまたは初期化
    EM_loadEEPROM();

    // イベントハンドラ登録
    // TCB0 は周期割り込み (INT) モードのため、TOP (CCMP) 到達で立つのは CAPT フラグ
    // (OVF は 0xFFFF からのラップでのみ立ち、INT モードでは発生しない)。
    TCB0_CaptureCallbackRegister(msecHandler);  // 1msecタイマ
    RTC_SetOVFIsrCallback(secHandler);          // 1secタイマ (MQTT publish 要求)

    //WIZnetドライバにコールバック関数を登録
	reg_wizchip_cris_cbfunc(spi_cris_enter, spi_cris_exit);
	reg_wizchip_cs_cbfunc(spi_cs_select, spi_cs_deselect);
	reg_wizchip_spi_cbfunc(spi_read_byte, spi_write_byte);
	if (WIZCHIP.IF.SPI._read_burst && WIZCHIP.IF.SPI._write_burst) { // 関数ポインタが定義されていれば
		reg_wizchip_spiburst_cbfunc(spi_read_burst, spi_write_burst);
	}
    
    //割り込み再開
	sei();

    //W5500 のフル初期化 (wizchip_init + ネットワーク情報投入)。
    MA_initW5500();

    //Resetスイッチ押し込み中 (= GND) の場合には初期設定 (HTTP) モードへ。
    //それ以外は MQTT 通信モード (本番運用)。
    isInitMode = (IO_RESET_GetValue() == 0);

    // I²C SCL 波形観測時は以下を有効化する。引数は SCL 周波数 [kHz]。
    //runI2cClockDummy(10);
    
    if (isInitMode) runHttpServerMode();
    else            runMqttClientMode();
}

// SCL ピン (PF3 = TWI1 既定 SCL) を GPIO でオープンドレイン擬似制御する観測用関数。
// 実 I²C と同じく、Lo は MCU が能動的に引き込み、Hi は外付けプルアップ抵抗任せ。
// これにより、ケーブル長 (容量) とプルアップで決まる RC 充電カーブが波形に現れる。
//   - OUT=0 を固定。DIR=1 で Lo 駆動、DIR=0 で Hi-Z (= プルアップで Hi)。
//   - 外付けプルアップが無いと Hi に上がらないので注意 (実 I²C 配線前提)。
//
// 引数 freq_khz: 目標 SCL 周波数 [kHz]。F_CPU = 24 MHz 前提。
// 実用範囲はおおむね 1 kHz 〜 400 kHz (それ以上は配線の RC 限界に当たる)。
static void runI2cClockDummy(uint16_t freq_khz)
{
    if (freq_khz == 0) freq_khz = 1;  // 0除算回避

    // TWI1 を完全停止してピン制御を GPIO 側へ譲渡。
    TWI1.MCTRLA = 0;

    // PF3 を OUT=0 固定。DIR で出力/Hi-Z を切り替えてオープンドレインを擬似する。
    PORTF.OUTCLR = PIN3_bm;
    PORTF.DIRCLR = PIN3_bm;  // 初期 Hi-Z (= プルアップで Hi)

    // 半周期分のループ回数。_delay_loop_2 は 4 cycle/iter。
    //   iter = F_CPU / (4 * 2 * F_SCL)
    //        = 24_000_000 / (8 * freq_khz * 1000)
    //        = 3000 / freq_khz
    // (DIR アクセスや wdt_reset 分の数 cycle は誤差として無視。)
    uint16_t half_loops = (uint16_t)(3000U / freq_khz);
    if (half_loops == 0) half_loops = 1;

    turnOffRedLED();
    turnOnGreenLED();

    while (true) {
        wdt_reset();
        PORTF.DIRCLR = PIN3_bm;       // Hi-Z → 外付けプルアップで Hi (RC 充電)
        _delay_loop_2(half_loops);
        PORTF.DIRSET = PIN3_bm;       // 出力 (OUT=0) → Lo を能動駆動
        _delay_loop_2(half_loops);
    }
}

// SCAN ボタン (IO_SCAN, PF0、プルアップ → 押下で Low) のエッジ検出。
// 立ち下がり (Hi→Lo) を検知したら true を返す。
static bool scanButtonPressed(void)
{
    static uint8_t prev_level = 1; // 起動時はプルアップで Hi
    uint8_t level = IO_SCAN_GetValue() ? 1 : 0;

    bool pressed = (prev_level == 1 && level == 0);
    prev_level = level;

    if (pressed) {
        // チャタリング対策: 短時間後に再確認
        DELAY_milliseconds(SCAN_DEBOUNCE_MS);
        if (IO_SCAN_GetValue()) {
            // ノイズだった
            prev_level = 1;
            return false;
        }
    }
    return pressed;
}

// 1 子機分の config を publish する (MQTT v2: poem/<pod>/node/<adr>/config)。
// 名前 (UTF-8 63 B) と最大 16 チャネルの記述が入るので、1 KB の静的バッファを使う
// (スタックに置くと深い呼び出しで足りなくなるおそれがある)。
static void publishConfigOne(slave_node_t* node)
{
    static char payload[1024];
    char topic[48];

    NM_buildTopic(node, MA_PodId(), "config", topic, sizeof(topic));
    uint16_t len = NM_buildConfigJson(node, payload, sizeof(payload));
    if (len == 0) return;
    MA_publish(topic, (const uint8_t*)payload, len);
    wdt_reset();
}

// 親機自身の情報を publish する (poem/<pod>/pod/info)。接続のたびに送る。
// name は Web 画面で設定する親機名 (EEPROM の topic_suffix 欄を使う)。
static void publishPodInfo(void)
{
    char topic[40];
    char payload[128];
    const uint8_t* ip = EM_Network_Settings.ip;

    // 親機名は Web 画面で自由に入力されるので、JSON を壊す文字 (" \ 制御文字) を _ に置き換え、
    // 多バイト文字の途中で切れていれば切り詰める (不正な UTF-8 だとサーバが JSON を読めない)
    char name[sizeof(EM_Network_Settings.topic_suffix)];
    strncpy(name, EM_Network_Settings.topic_suffix, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    for (char* c = name; *c; c++) {
        if (*c == '"' || *c == '\\' || (uint8_t)*c < 0x20) *c = '_';
    }
    NM_truncateUtf8(name, (uint16_t)(strlen(name)));

    snprintf(topic, sizeof(topic), "poem/%s/pod/info", MA_PodId());
    int len = snprintf(payload, sizeof(payload),
                       "{\"fw\":\"%u.%u.%u\",\"name\":\"%s\",\"ip\":\"%u.%u.%u.%u\"}",
                       POD_FW_MAJOR, POD_FW_MINOR, POD_FW_PATCH,
                       name,
                       ip[0], ip[1], ip[2], ip[3]);
    if (len <= 0 || (size_t)len >= sizeof(payload)) return;
    MA_publish(topic, (const uint8_t*)payload, (uint16_t)len);
    wdt_reset();
}

// pod 自身の I2C 状態を publish する。
//
// トピックは poem/<pod>/pod/diag。PoEMServer はこれをログに出すだけで、判定には使わない。
// バス固着が再発したときに MSTATUS と復帰回数を確認するための診断用。
//
// フィールドの意味:
//   mst  現在の MSTATUS       fail 失敗の合計 (= to + sf)   rcv  復帰の実行回数
//   to   タイムアウトでの失敗  sf   転送開始できずの失敗
//   fm   失敗時の MSTATUS (復帰処理で消される前の値。これだけが原因を示す)
//   scn  スキャンの実行回数 (総当たりなので、走ると fail が一気に 100 前後増える)
static void publishDiag(void)
{
    i2c_diag_t d;
    I2C_GetDiag(&d);

    // 最悪値 (すべて 5 桁) でも 125 文字 + NUL。余裕をみて 160。
    char payload[160];
    int len = snprintf(payload, sizeof(payload),
                       "{\"mst\":%u,\"fail\":%u,\"rcv\":%u,"
                       "\"to\":%u,\"sf\":%u,\"lost\":%u,\"fm\":%u,\"scn\":%u,"
                       "\"sdl\":%u,\"scl\":%u}",
                       (unsigned)d.mstatus, (unsigned)d.failCount, (unsigned)d.recoverCount,
                       (unsigned)d.failTimeout, (unsigned)d.failStart,
                       (unsigned)d.nodeLostCount,
                       (unsigned)d.failMstatus, (unsigned)NM_ScanCount,
                       (unsigned)d.sdaLowCount, (unsigned)d.sclLowCount);

    // 切り詰められた JSON を publish すると、サーバ側で壊れた行になる。
    // 過去に NaN で JSON を壊した経験があるので、ここでも落としておく。
    if (len <= 0 || (size_t)len >= sizeof(payload)) return;

    char topic[40];
    snprintf(topic, sizeof(topic), "poem/%s/pod/diag", MA_PodId());
    MA_publish(topic, (const uint8_t*)payload, (uint16_t)len);
    wdt_reset();
}

// 検出した全子機について config トピックを 1 回ずつ publish する。
static void publishAllConfigs(void)
{
    for (uint8_t i = 0; i < NM_NodeCount; i++) {
        publishConfigOne(&NM_Nodes[i]);
    }
}

// SCAN ボタンが押されたときの処理: I²C 全走査 → config 全送信 → 緑 LED 点滅で通知。
static void handleScanRequest(void)
{
    blinkGreenLED(1);
    publishPodInfo();
    NM_scan();
    publishAllConfigs();
    blinkGreenLED(2);
}

// 子機の拡張ステータス (稼働時間・起動要因・異常回数) を読み、変化があれば publish する。
//
// I2C は 2 本を全員で共有しているので、1 台が SCL を解放しないとバス上の全ノードが止まる。
// 症状は全台同じに見えて親機からは区別できないため、子機自身に数えさせてここで運ぶ。
// 変化したとき (異常回数が増えた、稼働時間が巻き戻った = 再起動した) だけ出すので、
// 平常時はまったく流れない。拡張ステータスを実装していない子機は何もしない。
//
// トピックはサーバにとって未知なのでログに出るだけで、判定処理には影響しない。
static void checkNodeDiag(slave_node_t* node)
{
    uint32_t up;
    uint8_t  rc;
    uint16_t ei, es;
    if (!NM_readExtStatus(node, &up, &rc, &ei, &es)) return;

    bool rebooted = (up < node->uptime_s);
    bool changed  = rebooted || ei != node->err_i2c || es != node->err_sensor;
    bool first    = (node->uptime_s == 0 && node->err_i2c == 0 && node->err_sensor == 0);
    node->uptime_s   = up;
    node->err_i2c    = ei;
    node->err_sensor = es;
    if (!changed || first) return;

    char    topic[48];
    uint8_t payload[80];
    NM_buildTopic(node, MA_PodId(), "diag", topic, sizeof(topic));

    int len = snprintf((char*)payload, sizeof(payload),
                       "{\"up\":%lu,\"rc\":%u,\"err\":%u,\"serr\":%u}",
                       (unsigned long)up, (unsigned)rc, (unsigned)ei, (unsigned)es);
    if (len <= 0 || (size_t)len >= sizeof(payload)) return;

    MA_publish(topic, payload, (uint16_t)len);
    wdt_reset();
}

// 1 ノードの POLL を読み、その場で data を publish する (poem/<pod>/node/<adr>/data)。
// 読めなかったとき (I2C 失敗、CRC 不一致) は全チャネルを通信失敗 (12) とし、値は送らない。
// リストからは外さず、生存確認のため publish を続ける。
static void pollAndPublishOne(slave_node_t* node)
{
    char topic[48];
    char payload[384];      // 16 チャネル: 値 14 字 × 16 + 状態 3 字 × 16 + 枠 ≒ 300 字

    bool     was_ok = node->comm_ok;
    uint16_t len;
    if (!NM_pollAndBuildDataJson(node, payload, sizeof(payload), &len)) {
        // 読めなくなった瞬間だけ数える (毎周期数えると、抜いている時間の長さが件数になる)
        if (was_ok) I2C_CountNodeLost();
    }

    NM_buildTopic(node, MA_PodId(), "data", topic, sizeof(topic));
    if (len > 0) {
        MA_publish(topic, (const uint8_t*)payload, len);
    }
}

// MilliTimer (1ms 毎インクリメント) を割り込みとの競合なしに読み出す。
static uint32_t readMilliTimer(void)
{
    uint32_t t;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { t = MilliTimer; }
    return t;
}

// 完全初期化: ネットワーク設定 (IP/SN/GW/DNS/ブローカーIP/トピック名) をデフォルト値へ
// 戻して EEPROM に保存し、Reset スイッチが離されるまで赤 LED 点滅で通知する。
// 押下中に再起動すると再び HTTP 設定モードへ入ってしまうため、解放を待ってから再起動する。
// この関数は戻らない。
static void factoryReset(void)
{
    EM_initNetworkSettings();
    EM_updateEEPROM();

    while (true) {
        wdt_reset();
        if (IO_RESET_GetValue() == 0) {
            blinkRedLED(1);   // 押下中は点滅を継続して初期化完了を通知
            continue;
        }
        // チャタリングで解放と誤認したまま再起動しないよう、安定を確認する
        DELAY_milliseconds(SCAN_DEBOUNCE_MS);
        if (IO_RESET_GetValue() != 0) break;
    }

    // ソフトウェアリセット (Reset は解放済みなので MQTT 通信モードで立ち上がる)
    ccp_write_io((void*)&RSTCTRL.SWRR, RSTCTRL_SWRST_bm);
}

// HTTP 設定モード: Reset スイッチ押し込み起動時のみ動作。Web 設定 UI を提供する。
// MQTT は走らせない (HTTP と並走させると W5500 のソケット混線が発生するため)。
// Reset スイッチを一度離した後、改めて 5 秒長押しすると完全初期化 → 再起動する。
static void runHttpServerMode(void)
{
    uint8_t http_socklist[] = {1};

    NM_init();   // /scan.cgi で書き込めるように初期化

    // 送受信バッファは MQTT 通信モードと共用する (このモードの間は MQTT を動かさない)
    httpServer_init(g_io_tx_buf, g_io_rx_buf, 1, http_socklist);
    reg_httpServer_webContent((uint8_t *)"index.html", (uint8_t *)index_page_content);

    turnOnRedLED();    // 設定モード中は赤 LED 常時点灯
    turnOffGreenLED();

    // 完全初期化 (Reset 長押し) の検出状態。
    // モード突入時はまだ起動時の押し込みが継続しているため、一度離されてから判定を開始する。
    bool release_seen = false;
    bool pressing = false;
    uint32_t press_start_ms = 0;

    while (true) {
        wdt_reset();
        httpServer_run(http_socklist[0]);

        bool pressed = (IO_RESET_GetValue() == 0);
        if (!release_seen) {
            release_seen = !pressed;   // 起動時の押し込みの解放待ち
        } else if (!pressed) {
            pressing = false;
        } else if (!pressing) {
            pressing = true;
            press_start_ms = readMilliTimer();
        } else if (readMilliTimer() - press_start_ms >= FACTORY_RESET_HOLD_MS) {
            factoryReset();   // 戻らない (再起動する)
        }
    }
}

// 新規子機の自動検出 (増分スキャン) の実行周期 [s]。
// 全アドレス走査 (SCAN ボタン相当) と違い既存子機の状態を壊さないため、定期実行できる。
#define AUTOSCAN_PERIOD_SEC 10

// MQTT 通信モード: 通常起動 (Reset 非押下) 時の本番運用。
//   - 起動後ブローカへ接続。失敗中は赤 LED が 2 回点滅で通知。
//   - 接続確立のたび (初回 + 再接続) に全走査 + config 全送信。
//     ブローカ (PoEMServer) 側が再起動して子機情報を失っていても自動復旧する。
//   - 1 秒周期で全子機を I²C ポーリング → MQTT publish。完了で緑 LED 1 回点滅。
//   - AUTOSCAN_PERIOD_SEC ごとに増分スキャンし、増設された子機の config を動的に送信。
//   - SCAN ボタンで子機再走査 + config 再送信 (緑 LED 点滅で通知)。
static void runMqttClientMode(void)
{
    uint8_t autoscan_countdown = AUTOSCAN_PERIOD_SEC;

    NM_init();

    // 初回接続: 成功するまでブロック (赤 LED 2 回点滅で外部に状況通知)
    while (!MA_connect()) {
        blinkRedLED(2);
        DELAY_milliseconds(2000);
        wdt_reset();
    }

    // 接続確立: 子機を検出して config を送信 (サーバ側の登録を確立する)
    handleScanRequest();

    while (true) {
        wdt_reset();

        // 接続切れ → 復帰待ちループ (赤 LED 3 回点滅)
        if (!MA_isConnected()) {
            MA_disconnect();
            while (!MA_connect()) {
                blinkRedLED(3);
                DELAY_milliseconds(3000);
                wdt_reset();
            }
            // 再接続成立: サーバが再起動して子機情報を失っている可能性があるため
            // 全走査 + config 再送信で登録を回復する
            handleScanRequest();
        }

        // MQTT keepalive 兼 受信処理
        MA_yield(10);

        // SCAN ボタン: 子機の再走査 + config 再送信
        if (scanButtonPressed()) {
            handleScanRequest();
        }

        // 1 秒タイマー: 全子機ポーリング → publish
        if (g_publish_request) {
            g_publish_request = false;
            for (uint8_t i = 0; i < NM_NodeCount; i++) {
                pollAndPublishOne(&NM_Nodes[i]);
                wdt_reset();
            }
            publishDiag();      // pod 自身の I2C 状態 (診断用)
            blinkGreenLED(1);   // 1 周期分の publish 完了通知

            // 増設された子機の自動検出: 新規分だけ config を送信する
            if (--autoscan_countdown == 0) {
                autoscan_countdown = AUTOSCAN_PERIOD_SEC;
                uint8_t added = NM_scanNewNodes();
                for (uint8_t i = NM_NodeCount - added; i < NM_NodeCount; i++) {
                    publishConfigOne(&NM_Nodes[i]);
                }
                if (added > 0) blinkGreenLED(2);   // 増設検出の通知

                // 子機の拡張ステータスも同じ周期で確かめる (再起動・異常回数の変化)
                for (uint8_t i = 0; i < NM_NodeCount; i++) {
                    checkNodeDiag(&NM_Nodes[i]);
                    wdt_reset();
                }
            }
        }
    }
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

// BUFEN モード前提の SPI バイトアクセス。
//   - DREIF: 送信バッファ空き (次のデータを書き込み可能)
//   - RXCIF: 受信完了 (SPI0.DATA を読むとクリア)

// 1バイトSPI読み出し (ダミー 0xFF 送信で受信を駆動)
uint8_t spi_read_byte(void) {
	while (!(SPI0.INTFLAGS & SPI_DREIF_bm));
	SPI0.DATA = 0xFF;
	while (!(SPI0.INTFLAGS & SPI_RXCIF_bm));
	return SPI0.DATA;
}

// 1バイトSPI書き込み (受信側はクリアのため捨てる)
void spi_write_byte(uint8_t data) {
	while (!(SPI0.INTFLAGS & SPI_DREIF_bm));
	SPI0.DATA = data;
	while (!(SPI0.INTFLAGS & SPI_RXCIF_bm));
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