/*
 * AVR128DB32 による差圧計子機 (PoEM 用 OSL v2 子機)。
 * SDP810 (Sensirion) を TWI1 マスタで読み取り、OSL 共通レジスタ v2 で
 * TWI0 スレーブとして親機に提供する。共通部は ../osl_common/osl_slave.c。
 *
 * ch0 = 差圧 [Pa]         (BACnet unit 53)
 * ch1 = センサ温度 [°C]   (BACnet unit 62、シリコン温度。気温ではない)
 *
 * 125Pa 品 (SDP810-125Pa) と 500Pa 品 (SDP810-500Pa) を同じファームウェアで扱う。
 * 起動時にセンサが返す換算係数 (240 / 60) で品種を判別し、model_id を変える。
 *
 * ユーザー領域 (0x1000〜) に線形補正係数 (差圧・温度の a, b) を置き、
 * corrected = a * raw + b で補正する。ゼロ点の合わせ込みは dp_b を書く。
 *
 * この基板にはリセットスイッチも LED も無い。
 */

// <editor-fold defaultstate="collapsed" desc="include">

#include "mcc_generated_files/system/system.h"
#include "mcc_generated_files/system/pins.h"
#include "mcc_generated_files/timer/rtc.h"
#include "mcc_generated_files/timer/delay.h"
#include "mcc_generated_files/i2c_host/twi1.h"

#include "sdp8xx.h"
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
#define MODEL_DP_125PA      0x0302
#define MODEL_DP_500PA      0x0303

// 換算係数 (SDP8xx が返す値)
#define SF_125PA            240
#define SF_500PA            60

// ===== 計測周期 ============================================================
// 125 ms 周期で計測し、直近 8 個の移動平均を公開する。
// 125 ms × 8 = 1.000 秒で、親機のポーリング周期とちょうど一致する。平均の窓が
// 隙間なく連続するので、どの計測もきっかり 1 回だけ親機に届く。
// RTC も 32768/8 = 4096 で割り切れる。
#define TICK_MS             125
#define TICKS_PER_SEC       (1000 / TICK_MS)
#define RTC_PERIOD_TICK     (4095)  // 32768 / TICKS_PER_SEC - 1
#define AVG_N               (8)

// SDP8xx の読み出しがこの回数 (= 1 秒) 続けて失敗したら連続測定を開始し直す。
// センサだけが電源瞬断などで再起動すると、停止状態に戻って読み出しに NACK を
// 返し続ける。開始コマンドを送り直さない限り復帰しない。
#define SENSOR_RESTART_FAILS (TICKS_PER_SEC)

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="OSL の設定">

// ユーザー領域 (0x1000〜)。並びは v1 の拡張領域と同じ。
typedef struct {
    float dp_a;     // 0x1000 差圧補正係数 a
    float dp_b;     // 0x1004 差圧補正係数 b [Pa] (ゼロ点補正に使う)
    float t_a;      // 0x1008 温度補正係数 a
    float t_b;      // 0x100C 温度補正係数 b [°C]
} user_region_t;

static user_region_t       s_user;
static const user_region_t s_user_default = { 1.0f, 0.0f, 1.0f, 0.0f };

static const osl_channel_t s_channels[2] = {
    { 53, OSL_TYPE_FLOAT32, "DP",   4, 0           },   // 差圧 [Pa]
    { 62, OSL_TYPE_FLOAT32, "Temp", 4, OSL_CHF_AUX },   // センサ温度 [°C]
};

// model_id は起動時にセンサの品種で決めるので const にしない
static osl_config_t s_osl = {
    .vendor_id        = OSL_VENDOR_POEM,
    .model_id         = MODEL_DP_125PA,
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

// 移動平均のリングバッファ (SDP8xx の生値)。
// 補正は一次式なので、平均してから補正しても補正してから平均しても同じ。
// float 演算が減る前者にする。
//
// センサ側の "average till read" は 25 ms を超えると α=0.05 (0.5 ms 刻み) の
// 指数平滑に切り替わるので、125 ms ごとに読んでも直近 20 ms 程度の平均でしかない。
// 1 秒の平均はここで作る。
static float   s_buf_dp[AVG_N];
static float   s_buf_t[AVG_N];
static uint8_t s_buf_idx    = 0;
static uint8_t s_buf_filled = 0;
static uint8_t s_fail_count = 0;

static meas_result_t measure(float *out_dp, float *out_t)
{
    float raw_dp = 0.0f;
    float raw_t  = 0.0f;

    if (!SDP8xx_ReadValue(&raw_dp, &raw_t)) {
        if (++s_fail_count >= SENSOR_RESTART_FAILS) {
            s_fail_count = 0;
            s_buf_filled = 0;   // 再開直後の値は数 % ずれうるので、平均もやり直す
            SDP8xx_Initialize();
        }
        return MEAS_FAIL;
    }
    s_fail_count = 0;

    s_buf_dp[s_buf_idx] = raw_dp;
    s_buf_t[s_buf_idx]  = raw_t;
    s_buf_idx = (uint8_t)((s_buf_idx + 1) % AVG_N);
    if (s_buf_filled < AVG_N) {
        s_buf_filled++;
        return MEAS_NOT_READY;
    }

    // 毎回 8 個を足し直す。差分更新は速いが float の丸め誤差が何時間も積もる。
    float sum_dp = 0.0f;
    float sum_t  = 0.0f;
    for (uint8_t i = 0; i < AVG_N; i++) {
        sum_dp += s_buf_dp[i];
        sum_t  += s_buf_t[i];
    }

    // 補正係数は親機が I2C で書き換えるので、まとめて取り出す
    user_region_t u;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        u = s_user;
    }

    *out_dp = u.dp_a * (sum_dp / AVG_N) + u.dp_b;
    *out_t  = u.t_a  * (sum_t  / AVG_N) + u.t_b;
    return MEAS_OK;
}

// 起動時にセンサの品種を判別する。換算係数が読めなければ 125Pa 品とみなす。
static uint16_t detect_model(void)
{
    float dp, t;
    for (uint8_t i = 0; i < 10; i++) {
        if (SDP8xx_ReadValue(&dp, &t)) break;
        DELAY_milliseconds(10);
    }
    return (SDP8xx_LastScaleFactor() == SF_500PA) ? MODEL_DP_500PA : MODEL_DP_125PA;
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

    // SDP8xx 用 TWI1 host の初期化 (MCC の SYSTEM_Initialize には含まれていない)
    TWI1_Initialize();

    // センサの連続測定を開始し、品種を判別してから OSL を初期化する
    // (model_id は識別区画に入り、CRC も起動時に計算するため)
    SDP8xx_Initialize();
    s_osl.model_id = detect_model();
    OSL_Init(&s_osl);

    // RTC を 1 秒周期から 125 ms 周期へ。PER を縮める前に CNT を戻さないと、
    // CNT > PER のまま 0xFFFF まで回ってしまい、初回だけ 2 秒待つことになる。
    RTC_WriteCounter(0);
    RTC_WritePeriod(RTC_PERIOD_TICK);
    RTC_SetOVFIsrCallback(tickHandler);

    set_sleep_mode(SLEEP_MODE_IDLE);
    sei();

    OSL_SetNotReady(true);

    while (1)
    {
        wdt_reset();
        OSL_Task();     // EEPROM への保存、コマンドの実行

        if (g_meas_tick)
        {
            g_meas_tick = false;
            OSL_Tick(TICK_MS);

            float  v[2];
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

        // SDP8xx は連続測定で動き続けるので、マイコンは IDLE スリープで割り込みを待つ
        sleep_mode();
    }
}

// </editor-fold>
