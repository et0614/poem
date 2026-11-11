/*
 * AVR128DB32 による絶対圧計子機 (PoEM 用 OSL v2 子機)。
 * BMP585 (Bosch Sensortec) を TWI1 マスタで読み取り、OSL 共通レジスタ v2 で
 * TWI0 スレーブとして親機に提供する。共通部は ../osl_common/osl_slave.c。
 *
 * ch0 = 絶対圧 [Pa]       (BACnet unit 53)
 * ch1 = センサ温度 [°C]   (BACnet unit 62、基板の温度。気温ではない)
 *
 * ユーザー領域 (0x1000〜) に線形補正係数 (気圧・温度の a, b) を置き、
 * corrected = a * raw + b で補正する。
 *
 * LED (接続確認用) は識別コマンドで 2 Hz で点滅させる (PoEM Link の識別と同じ速さ)。
 * 工場リセットのスイッチは持たない。上位機器からアドレス・名前・ユーザー領域を書き換えて戻す。
 */

// <editor-fold defaultstate="collapsed" desc="include">

#include "mcc_generated_files/system/system.h"
#include "mcc_generated_files/system/pins.h"
#include "mcc_generated_files/timer/rtc.h"
#include "mcc_generated_files/i2c_host/twi1.h"

#include "bmp585.h"
#include "osl_slave.h"

#include <util/atomic.h>
#include <avr/sleep.h>
#include <avr/wdt.h>

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="定数">

// ファームウェアの版
#define FW_MAJOR            2
#define FW_MINOR            0
#define FW_PATCH            0

// model_id (仕様 §6.1)
#define MODEL_AP_BMP585     0x0301

// ===== 計測周期 ============================================================
// 125 ms 周期で計測し、直近 8 個の移動平均を公開する (温湿度センサと同じ理由)。
#define TICK_MS             125
#define TICKS_PER_SEC       (1000 / TICK_MS)
#define RTC_PERIOD_TICK     (4095)  // 32768 / TICKS_PER_SEC - 1
#define AVG_N               (8)

// BMP585 の読み出しがこの回数 (= 1 秒) 続けて失敗したら初期化し直す
#define SENSOR_RESTART_FAILS (TICKS_PER_SEC)

// 識別の点滅: 2 計測周期 (250 ms) ごとに点灯と消灯を入れ替える = 2 Hz
#define IDENTIFY_HALF_TICKS (2)

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="OSL の設定">

// ユーザー領域 (0x1000〜)。並びは v1 の拡張領域と同じ。
typedef struct {
    float p_a;      // 0x1000 気圧補正係数 a
    float p_b;      // 0x1004 気圧補正係数 b [Pa]
    float t_a;      // 0x1008 温度補正係数 a
    float t_b;      // 0x100C 温度補正係数 b [°C]
} user_region_t;

static user_region_t       s_user;
static const user_region_t s_user_default = { 1.0f, 0.0f, 1.0f, 0.0f };

// 識別の点滅の残り [計測周期]。OSL_Task とメインループの計測処理だけが触る (どちらも割り込みの外)
static uint16_t s_identify_ticks = 0;

// 識別コマンド (OSL_Task から)。seconds = 0 は停止。
// 秒数は最大 255 なので、計測周期の数 (最大 2040) で数える (ms だと uint16 からあふれる)。
static void onIdentify(uint8_t seconds)
{
    s_identify_ticks = (uint16_t)seconds * TICKS_PER_SEC;
    if (s_identify_ticks == 0) LED_SetLow();
}

static const osl_channel_t s_channels[2] = {
    { 53, OSL_TYPE_FLOAT32, "Press", 6, 0           },   // 絶対圧 [Pa]
    { 62, OSL_TYPE_FLOAT32, "Temp",  4, OSL_CHF_AUX },   // センサ温度 [°C]
};

static const osl_config_t s_osl = {
    .vendor_id        = OSL_VENDOR_POEM,
    .model_id         = MODEL_AP_BMP585,
    .fw_major         = FW_MAJOR,
    .fw_minor         = FW_MINOR,
    .fw_patch         = FW_PATCH,
    .update_period_ms = TICK_MS,
    .channel_count    = 2,
    .channels         = s_channels,
    .capability       = OSL_CAP_CH_ATTR | OSL_CAP_EXT_STATUS | OSL_CAP_CMD_RESET
                      | OSL_CAP_CMD_IDENTIFY,
    .hw_rev           = 0,
    .avg_window_ms    = TICK_MS * AVG_N,
    .warmup_ms        = TICK_MS * AVG_N,
    .default_i2c_addr = OSL_DEFAULT_I2C_ADDR,
    .user_region      = &s_user,
    .user_size        = sizeof(s_user),
    .user_writable    = true,
    .user_default     = &s_user_default,
    .on_identify      = onIdentify,
    .on_user_changed  = NULL,
};

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="計測処理">

typedef enum {
    MEAS_OK,            // 平均が出た
    MEAS_NOT_READY,     // 読めたが、平均の窓がまだ埋まっていない
    MEAS_FAIL,          // センサを読めなかった
} meas_result_t;

// 計測周期ごとに立つ計測要求フラグ (RTC OVF で true に)
static volatile bool g_meas_tick = false;

// 移動平均のリングバッファ (BMP585 の生値)。
// 気圧 (約 1e5 Pa) を 8 個足すと 8e5 になり、float の刻みは 0.0625 Pa。
// 8 で割って 0.008 Pa なので、センサの雑音 (0.1 Pa 未満) より十分細かい。
static float   s_buf_p[AVG_N];
static float   s_buf_t[AVG_N];
static uint8_t s_buf_idx    = 0;
static uint8_t s_buf_filled = 0;
static uint8_t s_fail_count = 0;

// BMP585 を初期化し直す。移動平均もやり直す。
static void restartSensor(void)
{
    s_fail_count = 0;
    s_buf_filled = 0;
    BMP585_Initialize();
}

static meas_result_t measure(float *out_p, float *out_t)
{
    float raw_p = 0.0f;
    float raw_t = 0.0f;

    if (!BMP585_ReadValue(&raw_p, &raw_t)) {
        if (++s_fail_count >= SENSOR_RESTART_FAILS) restartSensor();
        return MEAS_FAIL;
    }
    s_fail_count = 0;

    s_buf_p[s_buf_idx] = raw_p;
    s_buf_t[s_buf_idx] = raw_t;
    s_buf_idx = (uint8_t)((s_buf_idx + 1) % AVG_N);
    if (s_buf_filled < AVG_N) {
        s_buf_filled++;
        return MEAS_NOT_READY;
    }

    // 毎回 8 個を足し直す。差分更新は速いが float の丸め誤差が何時間も積もる。
    float sum_p = 0.0f;
    float sum_t = 0.0f;
    for (uint8_t i = 0; i < AVG_N; i++) {
        sum_p += s_buf_p[i];
        sum_t += s_buf_t[i];
    }

    // 補正係数は親機が I2C で書き換えるので、まとめて取り出す
    user_region_t u;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        u = s_user;
    }

    *out_p = u.p_a * (sum_p / AVG_N) + u.p_b;
    *out_t = u.t_a * (sum_t / AVG_N) + u.t_b;
    return MEAS_OK;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="main">

// 計測タイマ用コールバック (RTC OVF, 4096/32768 = 125 ms)
static void tickHandler(void)
{
    g_meas_tick = true;
}

int main(void)
{
    // WDT は SYSTEM_Initialize() 内で 4.1 秒周期に設定される。メインループで
    // 必ず wdt_reset() を呼ぶこと。
    SYSTEM_Initialize();

    // BMP585 用 TWI1 host の初期化 (MCC の SYSTEM_Initialize には含まれていない)
    TWI1_Initialize();

    OSL_Init(&s_osl);

    // BMP585 初期化 (失敗しても measure() が 1 秒ごとに初期化し直す)
    BMP585_Initialize();

    // RTC を 1 秒周期から 125 ms 周期へ。PER を縮める前に CNT を戻さないと、
    // CNT > PER のまま 0xFFFF まで回ってしまい、初回だけ 2 秒待つことになる。
    RTC_WriteCounter(0);
    RTC_WritePeriod(RTC_PERIOD_TICK);
    RTC_SetOVFIsrCallback(tickHandler);

    set_sleep_mode(SLEEP_MODE_IDLE);
    sei();

    OSL_SetNotReady(true);
    uint8_t tick_in_sec    = 0;

    while (1)
    {
        wdt_reset();
        OSL_Task();     // EEPROM への保存、コマンドの実行

        if (g_meas_tick)
        {
            g_meas_tick = false;
            OSL_Tick(TICK_MS);


            // 識別の点滅 (2 Hz)。最後は必ず消灯で終わる
            if (s_identify_ticks) {
                s_identify_ticks--;
                if (s_identify_ticks && (s_identify_ticks / IDENTIFY_HALF_TICKS) & 1) LED_SetHigh();
                else                                                                 LED_SetLow();
            }

            // BMP585 がひとりで再起動していないかを 1 秒ごとに確かめる。
            // 電源の瞬断などで再起動すると測定が止まり、データレジスタには最後の値が
            // 残ったままになる。読み出しは成功し続けるので、失敗回数では気づけない。
            if (++tick_in_sec >= TICKS_PER_SEC) {
                tick_in_sec = 0;
                if (BMP585_NeedsRestart()) restartSensor();
            }

            float   v[2];
            uint8_t st[2];
            switch (measure(&v[0], &v[1])) {
                case MEAS_OK:
                    st[0] = st[1] = OSL_ST_OK;
                    OSL_Publish(v, st);
                    OSL_SetNotReady(false);
                    break;
                case MEAS_NOT_READY:
                    st[0] = st[1] = OSL_ST_NOT_READY;
                    OSL_Publish(NULL, st);
                    OSL_SetNotReady(true);
                    break;
                default:
                    st[0] = st[1] = OSL_ST_NO_SENSOR;
                    OSL_Publish(NULL, st);
                    OSL_CountSensorError();
                    break;
            }
        }

        // BMP585 は連続測定で動き続けるので、マイコンは IDLE スリープで割り込みを待つ
        sleep_mode();
    }
}

// </editor-fold>
