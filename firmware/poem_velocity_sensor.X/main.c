 /*
 * AVR128DB32 による風速計子機 (PoEM 用 OSL v2 子機)。
 * 定温熱線式風速計の電圧を読み取り、風速 [m/s] に換算して OSL 共通レジスタ v2 で
 * TWI0 スレーブとして親機に提供する。共通部は ../osl_common/osl_slave.c。
 *
 * ch0 = 風速 [m/s]   (BACnet unit 74)
 * ch1 = 生電圧 [V]   (BACnet unit 5、補助値)
 *
 * ユーザー領域 (0x1000〜):
 *   0x1000 enable (u8)    風速計回路 (5V 昇圧・熱線) の入切。起動時は必ず 1 にする
 *   0x1001 filter_n (u8)  平滑化フィルタの係数 (0〜20)
 *   0x1004 E0, m1, lnC1, m2, lnC2 (float×5)  King の法則の係数 (v1 の coefficientA)
 *   0x1018 m3, lnC3, vSplit1, vSplit2 (float×4)  (v1 の coefficientB[0..3])
 *
 * 計測は 20 ms 周期 (ADC + 指数平滑)。親機には 500 ms ごとに新しい値を公開する。
 * LED (接続確認用、PD5) は識別コマンドで 2 Hz で点滅させる (PoEM Link・ほかの子機と同じ速さ)。
 * 工場リセットのスイッチは持たない。上位機器からアドレス・名前・ユーザー領域を書き換えて戻す。
 */

// <editor-fold defaultstate="collapsed" desc="include">

#include "mcc_generated_files/system/system.h"
#include "mcc_generated_files/adc/adc0.h"
#include "mcc_generated_files/system/pins.h"
#include "mcc_generated_files/timer/rtc.h"

#include "smooth_filter.h"
#include "osl_slave.h"

#include <util/atomic.h>
#include <string.h>
#include <math.h>
#include <avr/sleep.h>
#include <avr/wdt.h>

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="定数">

// ファームウェアの版
#define FW_MAJOR        2
#define FW_MINOR        0
#define FW_PATCH        0

// model_id (仕様 §6.1)
#define MODEL_VEL_HOTWIRE   0x0201

#define HEATING_MSEC    (5000)  // 白金抵抗予熱時間 [msec]
#define TICK_MS         (20)    // タイマ割り込み間隔 [msec] (RTC PER = 654 → 32768/655 Hz)

// 新しい値を公開する周期 [ms]。計測そのものは 20 ms ごとに行い、指数平滑で均す。
#define PUBLISH_MS      (500)
#define PUBLISH_TICKS   (PUBLISH_MS / TICK_MS)

// 識別表示の点滅の半周期 [ms] (2 Hz)
#define IDENTIFY_HALF_MS (250)

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="OSL の設定">

#pragma pack(push, 1)
typedef struct {
    uint8_t enable;         // 0x1000
    uint8_t filter_n;       // 0x1001
    uint8_t reserved[2];    // 0x1002 float の整列用
    float   E0;             // 0x1004 無風電圧 [V]
    float   m1;             // 0x1008
    float   lnC1;           // 0x100C
    float   m2;             // 0x1010
    float   lnC2;           // 0x1014
    float   m3;             // 0x1018
    float   lnC3;           // 0x101C
    float   vSplit1;        // 0x1020 [m/s]
    float   vSplit2;        // 0x1024 [m/s]
} user_region_t;
#pragma pack(pop)

static user_region_t       s_user;
static const user_region_t s_user_default = {
    .enable   = 1,
    .filter_n = 6,
    .E0 = 0.462f, .m1 = 0.970f, .lnC1 = 0.183f, .m2 = 1.584f, .lnC2 = 0.865f,
    .m3 = 0.410f, .lnC3 = 0.0f, .vSplit1 = 0.0f, .vSplit2 = 0.0f,
};

static const osl_channel_t s_channels[2] = {
    { 74, OSL_TYPE_FLOAT32, "Vel",  3, 0           },   // 風速 [m/s]
    {  5, OSL_TYPE_FLOAT32, "Volt", 4, OSL_CHF_AUX },   // 風速計回路の生電圧 [V]
};

static void onIdentify(uint8_t seconds);
static void onUserChanged(void);

static const osl_config_t s_osl = {
    .vendor_id        = OSL_VENDOR_POEM,
    .model_id         = MODEL_VEL_HOTWIRE,
    .fw_major         = FW_MAJOR,
    .fw_minor         = FW_MINOR,
    .fw_patch         = FW_PATCH,
    .update_period_ms = PUBLISH_MS,
    .channel_count    = 2,
    .channels         = s_channels,
    .capability       = OSL_CAP_CH_ATTR | OSL_CAP_EXT_STATUS | OSL_CAP_CMD_RESET
                      | OSL_CAP_CMD_IDENTIFY,
    .hw_rev           = 0,
    .avg_window_ms    = 0,              // 平均ではなく指数平滑 (filter_n)
    .warmup_ms        = HEATING_MSEC,
    .default_i2c_addr = OSL_DEFAULT_I2C_ADDR,
    .user_region      = &s_user,
    .user_size        = sizeof(s_user),
    .user_writable    = true,
    .user_default     = &s_user_default,
    .on_identify      = onIdentify,
    .on_user_changed  = onUserChanged,
};

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="変数">

static volatile uint8_t tick_count = 0;
static uint16_t heating_timer = HEATING_MSEC;
static uint32_t identify_ms   = 0;      // 識別表示の残り時間 (255 秒 = 255000 ms は uint16 からあふれる)
static uint8_t  enabled       = 1;      // 現在の風速計回路の状態

// 風速電圧平滑化フィルタ
static SmoothFilter velFilter;

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="風速測定">

// 風速計回路の出力電圧を AD 変換 (mV, 0-2000)
static uint32_t adcVelocityVoltage(void)
{
    ADC0_ChannelSelect(ADC0_CHANNEL_AIN21);
    ADC0.SAMPCTRL = 0;
    ADC0.CTRLD |= ADC_INITDLY_DLY0_gc;
    ADC0_ConversionStart();
    while (!ADC0_IsConversionDone());
    adc_accumulate_t acc_val = ADC0_AccumulatedResultGet();

    uint32_t safe_val = (uint32_t)acc_val;
    // 12bit (4096), 128回平均 -> AVR の仕様で3bit切り捨てられるので 16 で割られる
    return safe_val * 2000UL / (4096UL * 16UL);
}

// 風速電圧を AD 変換して平滑化し、フィルタ出力 [V] を返す
static float sampleVelocityVoltage(void)
{
    uint32_t voltage_mv = adcVelocityVoltage();
    SF_Apply(&velFilter, (int32_t)voltage_mv);
    return (float)velFilter.out_y * 0.001f;
}

// 電圧 [V] から風速 [m/s] を計算する。
// King の法則 v = exp(m * ln(E^2 - E0^2) + lnC) を 3 区分でフィット。
static float computeVelocity(float voltage_v)
{
    user_region_t u;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        u = s_user;     // 係数は親機が I2C で書き換えるので、まとめて取り出す
    }

    float evv = voltage_v * voltage_v - u.E0 * u.E0;
    if (evv <= 0.0f) return 0.0f;

    float lnev  = logf(evv);
    float vel_f = expf(lnev * u.m1 + u.lnC1);
    if (u.vSplit1 <= vel_f) {
        vel_f = expf(lnev * u.m2 + u.lnC2);
        if (u.vSplit2 <= vel_f) vel_f = expf(lnev * u.m3 + u.lnC3);
    }
    return vel_f;
}

// 風速計回路の入切。入れたら予熱からやり直す。
static void applyEnable(uint8_t enable)
{
    if (enable && !enabled) heating_timer = HEATING_MSEC;
    enabled = enable ? 1 : 0;
    if (enabled) SLP_SetHigh();
    else         SLP_SetLow();
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="OSL のコールバック (割り込みの外)">

static void onIdentify(uint8_t seconds)
{
    identify_ms = (uint32_t)seconds * 1000UL;
    if (identify_ms == 0) LED_SetLow();
}

// 親機がユーザー領域を書き換えたとき: enable と filter_n を反映する
static void onUserChanged(void)
{
    uint8_t en, n;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        en = s_user.enable;
        n  = s_user.filter_n;
    }
    applyEnable(en);
    SF_Init(&velFilter, n, velFilter.out_y);
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="main">

// 20 ms タイマ用コールバック
static void tickHandler(void)
{
    if (tick_count < 255) tick_count++;
}

int main(void)
{
    // WDT は SYSTEM_Initialize() 内で 4.1 秒周期に設定される。メインループで
    // 必ず wdt_reset() を呼ぶこと。
    SYSTEM_Initialize();

    OSL_Init(&s_osl);

    // 風速計回路は起動時に必ず入れる (保存されていた enable によらない)。
    // 親機から給電されていて低消費電力の必要は無い。
    s_user.enable = 1;
    enabled = 0;
    applyEnable(1);

    SF_Init(&velFilter, s_user.filter_n, 0);
    RTC_SetOVFIsrCallback(tickHandler);

    set_sleep_mode(SLEEP_MODE_IDLE);
    sei();

    OSL_SetNotReady(true);
    uint8_t publish_div = 0;

    while (1)
    {
        wdt_reset();
        OSL_Task();     // EEPROM への保存、コマンドの実行、設定変更の反映

        if (tick_count)
        {
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { tick_count--; }
            OSL_Tick(TICK_MS);


            // 識別表示 (2 Hz の点滅)
            if (identify_ms) {
                identify_ms = (identify_ms < TICK_MS) ? 0 : identify_ms - TICK_MS;
                if (identify_ms && (identify_ms / IDENTIFY_HALF_MS) & 1) LED_SetHigh();
                else                                                     LED_SetLow();
            }

            // 残予熱時間
            heating_timer = (heating_timer < TICK_MS) ? 0 : (uint16_t)(heating_timer - TICK_MS);

            // 計測 (20 ms ごと)。予熱中と回路 OFF 中は計測しない。
            bool ready = enabled && heating_timer == 0;
            float v[2] = { 0.0f, 0.0f };
            if (ready) {
                v[1] = sampleVelocityVoltage();
                v[0] = computeVelocity(v[1]);
            }

            // 公開 (500 ms ごと)
            if (++publish_div >= PUBLISH_TICKS) {
                publish_div = 0;
                uint8_t st[2];
                if (ready) {
                    st[0] = st[1] = OSL_ST_OK;
                    OSL_Publish(v, st);
                } else {
                    // 予熱中は準備中、回路 OFF 中は「計測していない」ので異常として示す
                    st[0] = st[1] = enabled ? OSL_ST_NOT_READY : OSL_ST_UNRELIABLE;
                    OSL_Publish(NULL, st);
                }
                OSL_SetNotReady(enabled && !ready);
            }
        }

        // スリープは IDLE 固定。RTC が RUNSTDBY disabled のため、PWR_DOWN では
        // tick が来ず wdt_reset() を呼べなくなり、WDT で再起動してしまう。
        sleep_mode();
    }
}

// </editor-fold>
