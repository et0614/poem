/*
 * AVR128DB32 による温湿度計子機 (PoEM 用 OSL v2 子機)。
 * SHT4x (Sensirion) を TWI1 マスタで読み取り、OSL 共通レジスタ v2 で
 * TWI0 スレーブとして親機に提供する。共通部は ../osl_common/osl_slave.c。
 *
 * ch0 = 温度 [°C]        (BACnet unit 62)
 * ch1 = 相対湿度 [%RH]   (BACnet unit 29)
 *
 * ユーザー領域 (0x1000〜) に線形補正係数 (温度・湿度の a, b) を置き、
 * corrected = a * raw + b で補正する。
 *
 * リセットスイッチを 3 秒長押しすると工場出荷状態に戻る (アドレス 0x10、名前は空、
 * 補正係数は恒等)。
 */

// <editor-fold defaultstate="collapsed" desc="include">

#include "mcc_generated_files/system/system.h"
#include "mcc_generated_files/system/pins.h"
#include "mcc_generated_files/timer/rtc.h"
#include "mcc_generated_files/i2c_host/twi1.h"

#include "sht4x.h"
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
#define MODEL_TH_SHT4X      0x0101

// SHT4x の I2C アドレス (基板実装は SHT40-AD1B = 0x44)
#define SHT4X_TYPE          SHT4_AD

// ===== 計測周期 ============================================================
// 125 ms 周期で計測し、直近 8 個の移動平均を公開する。
// 125 ms × 8 = 1.000 秒で、親機のポーリング周期とちょうど一致する。平均の窓が
// 隙間なく連続するので、どの計測もきっかり 1 回だけ親機に届く。100 ms だと窓が
// 0.8 秒になり、1 秒ごとに 10 個進むうち 2 個が誰にも読まれずに捨てられる。
// RTC も 32768/8 = 4096 で割り切れる。
#define TICK_MS             125
#define TICKS_PER_SEC       (1000 / TICK_MS)
#define RTC_PERIOD_TICK     (4095)  // 32768 / TICKS_PER_SEC - 1
#define AVG_N               (8)

// リセットスイッチ長押しで工場出荷状態に戻す [秒]
#define RESET_HOLD_SECONDS  (3)
#define RESET_HOLD_TICKS    (RESET_HOLD_SECONDS * TICKS_PER_SEC)

// SHT4x の読み出しがこの回数 (= 1 秒) 続けて失敗したら初期化し直す
#define SENSOR_RESTART_FAILS (TICKS_PER_SEC)

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="OSL の設定">

// ユーザー領域 (0x1000〜)。並びは v1 の拡張領域と同じ。
typedef struct {
    float t_a;      // 0x1000 温度補正係数 a
    float t_b;      // 0x1004 温度補正係数 b [°C]
    float h_a;      // 0x1008 湿度補正係数 a
    float h_b;      // 0x100C 湿度補正係数 b [%RH]
} user_region_t;

static user_region_t       s_user;
static const user_region_t s_user_default = { 1.0f, 0.0f, 1.0f, 0.0f };

static const osl_channel_t s_channels[2] = {
    { 62, OSL_TYPE_FLOAT32, "Temp", 4, 0 },     // 温度 [°C]
    { 29, OSL_TYPE_FLOAT32, "RH",   4, 0 },     // 相対湿度 [%RH]
};

static const osl_config_t s_osl = {
    .vendor_id        = OSL_VENDOR_POEM,
    .model_id         = MODEL_TH_SHT4X,
    .fw_major         = FW_MAJOR,
    .fw_minor         = FW_MINOR,
    .fw_patch         = FW_PATCH,
    .update_period_ms = TICK_MS,
    .channel_count    = 2,
    .channels         = s_channels,
    .capability       = OSL_CAP_CH_ATTR | OSL_CAP_EXT_STATUS | OSL_CAP_CMD_RESET,
    .hw_rev           = 0,
    .avg_window_ms    = TICK_MS * AVG_N,
    .warmup_ms        = TICK_MS * AVG_N,
    .default_i2c_addr = OSL_DEFAULT_I2C_ADDR,
    .user_region      = &s_user,
    .user_size        = sizeof(s_user),
    .user_writable    = true,
    .user_default     = &s_user_default,
    .on_identify      = NULL,       // LED が無いので識別コマンドは持たない
    .on_user_changed  = NULL,       // 係数は計測のたびに読むので、変更時の処理は要らない
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

// 移動平均のリングバッファ (SHT4x の生値)。補正は一次式なので、平均してから
// 補正しても補正してから平均しても同じ。float 演算が減る前者にする。
static float   s_buf_t[AVG_N];
static float   s_buf_h[AVG_N];
static uint8_t s_buf_idx    = 0;
static uint8_t s_buf_filled = 0;
static uint8_t s_fail_count = 0;

static meas_result_t measure(float *out_t, float *out_h)
{
    float raw_t = 0.0f;
    float raw_h = 0.0f;

    if (!SHT4x_ReadValue(&raw_t, &raw_h, SHT4X_TYPE)) {
        if (++s_fail_count >= SENSOR_RESTART_FAILS) {
            s_fail_count = 0;
            s_buf_filled = 0;
            SHT4x_Initialize(SHT4X_TYPE);
        }
        return MEAS_FAIL;
    }
    s_fail_count = 0;

    s_buf_t[s_buf_idx] = raw_t;
    s_buf_h[s_buf_idx] = raw_h;
    s_buf_idx = (uint8_t)((s_buf_idx + 1) % AVG_N);
    if (s_buf_filled < AVG_N) {
        s_buf_filled++;
        return MEAS_NOT_READY;
    }

    // 毎回 8 個を足し直す。差分更新は速いが float の丸め誤差が何時間も積もる。
    float sum_t = 0.0f;
    float sum_h = 0.0f;
    for (uint8_t i = 0; i < AVG_N; i++) {
        sum_t += s_buf_t[i];
        sum_h += s_buf_h[i];
    }

    // 補正係数は親機が I2C で書き換えるので、まとめて取り出す
    user_region_t u;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        u = s_user;
    }

    *out_t = u.t_a * (sum_t / AVG_N) + u.t_b;
    *out_h = u.h_a * (sum_h / AVG_N) + u.h_b;
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
    // 必ず wdt_reset() を呼ぶこと (v1 では呼び忘れで 4.1 秒ごとに再起動していた)。
    SYSTEM_Initialize();

    // SHT4x 用 TWI1 host の初期化 (MCC の SYSTEM_Initialize には含まれていない)
    TWI1_Initialize();

    OSL_Init(&s_osl);

    // SHT4x 初期化 (失敗しても計測のたびに読み直し、1 秒続けば初期化し直す)
    SHT4x_Initialize(SHT4X_TYPE);

    // RTC を 1 秒周期から 125 ms 周期へ。PER を縮める前に CNT を戻さないと、
    // CNT > PER のまま 0xFFFF まで回ってしまい、初回だけ 2 秒待つことになる。
    RTC_WriteCounter(0);
    RTC_WritePeriod(RTC_PERIOD_TICK);
    RTC_SetOVFIsrCallback(tickHandler);

    set_sleep_mode(SLEEP_MODE_IDLE);
    sei();

    OSL_SetNotReady(true);
    uint8_t rst_hold_count = 0;

    while (1)
    {
        wdt_reset();
        OSL_Task();     // EEPROM への保存、コマンドの実行

        if (g_meas_tick)
        {
            g_meas_tick = false;
            OSL_Tick(TICK_MS);

            // リセットスイッチ長押し判定 (RESET_HOLD_SECONDS 秒で発動)
            if (IO_RESET_GetValue() == 0) {
                if (++rst_hold_count >= RESET_HOLD_TICKS) {
                    OSL_FactoryReset();
                    rst_hold_count = 0;
                }
            } else {
                rst_hold_count = 0;
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

        // SHT4x は常時起動 (idle 0.4µA) のため IDLE スリープに固定
        sleep_mode();
    }
}

// </editor-fold>
