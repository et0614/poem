/*
 * AVR128DB32 による絶対圧計子機 (PoEM-pod 用 OSL 子機)。
 * BMP585 (Bosch Sensortec) を TWI1 マスタで読み取り、共通レジスタ仕様
 * (SHASE 2026 第2報) で TWI0 スレーブとして親機に提供する。
 * 温湿度センサ (poem_temperature_sensor.X) からの派生で、センサ部以外は同じ構造。
 *
 * value[0] = 絶対圧 [Pa]       (BACnet unit 53)
 * value[1] = センサ温度 [°C]   (BACnet unit 62)
 *
 * 拡張領域には線形補正係数 (気圧・温度の a, b) を保持し、
 * corrected = a * raw + b の式で計測値を補正する。
 *
 * この基板に LED は実装していない。
 */

// <editor-fold defaultstate="collapsed" desc="include">

#include "mcc_generated_files/system/system.h"
#include "mcc_generated_files/system/pins.h"
#include "mcc_generated_files/timer/rtc.h"
#include "mcc_generated_files/i2c_host/twi1.h"

#include "utility.h"
#include "bmp585.h"
#include "i2c_slave.h"
#include "i2c_shared_data.h"
#include "eeprom_manager.h"

#include <util/atomic.h>
#include <string.h>
#include <avr/sleep.h>
#include <avr/wdt.h>

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="定数">

// ===== 計測周期 ============================================================
// 125 ms 周期で計測し、直近 8 個の移動平均を親機に返す。
//
// 125 ms × 8 = 1.000 秒 で、親機のポーリング周期とちょうど一致する。平均の窓が
// 隙間なく連続するので、どの計測もきっかり 1 回だけ親機に届く。100 ms だと窓が
// 0.8 秒になり、1 秒ごとに 10 個進むうち 2 個が誰にも読まれずに捨てられる。
// RTC も 32768/8 = 4096 で割り切れる。
#define TICKS_PER_SEC       (8)
#define RTC_PERIOD_TICK     (4095)  // 32768 / TICKS_PER_SEC - 1
#define AVG_N               (8)

// Reset スイッチ長押しで EEPROM 初期化 [秒]
#define RESET_HOLD_SECONDS  (3)
#define RESET_HOLD_TICKS    (RESET_HOLD_SECONDS * TICKS_PER_SEC)

// BMP585 の読み出しがこの回数 (= 1 秒) 続けて失敗したら初期化し直す
#define SENSOR_RESTART_FAILS (TICKS_PER_SEC)

// CLKHOLD が連続で立ちっぱなしなら異常とみなす閾値 (= 1 秒)
#define CLKHOLD_STUCK_TICKS (TICKS_PER_SEC)

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="変数">

// 計測周期ごとに立つ計測要求フラグ (RTC OVF で true に)
volatile bool g_meas_tick = false;

// 移動平均のリングバッファ (BMP585 の生値)。
// 補正は corrected = a*raw + b の一次式なので、平均してから補正しても
// 補正してから平均しても同じ。float 演算が減る前者にする。
//
// 気圧 (約 1e5 Pa) を 8 個足すと 8e5 になり、float の刻みは 0.0625 Pa。
// 8 で割って 0.008 Pa なので、センサの雑音 (0.1 Pa 未満) より十分細かい。
static float   s_buf_p[AVG_N];
static float   s_buf_t[AVG_N];
static uint8_t s_buf_idx    = 0;
static uint8_t s_buf_filled = 0;

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="計測処理">

// 読み出しの連続失敗回数 (SENSOR_RESTART_FAILS で初期化し直す)
static uint8_t s_fail_count = 0;

// BMP585 を初期化し直す。移動平均もやり直す。
static void restartSensor(void)
{
    s_fail_count = 0;
    s_buf_filled = 0;
    BMP585_Initialize();
}

// BMP585 から気圧・温度を読み取り、移動平均に積んで、補正を適用して返す。
// 窓が埋まるまで (起動後 1 秒) は平均が出せないので false を返す。
// SharedMemory への公開は行わない (公開タイミングは main loop が制御する)
static bool measure(float *out_p, float *out_t)
{
    float raw_p = 0.0f;
    float raw_t = 0.0f;

    if (!BMP585_ReadValue(&raw_p, &raw_t)) {
        if (++s_fail_count >= SENSOR_RESTART_FAILS) restartSensor();
        return false;
    }
    s_fail_count = 0;

    s_buf_p[s_buf_idx] = raw_p;
    s_buf_t[s_buf_idx] = raw_t;
    s_buf_idx = (uint8_t)((s_buf_idx + 1) % AVG_N);
    if (s_buf_filled < AVG_N) {
        s_buf_filled++;
        return false;   // まだ 8 個そろっていない
    }

    // 毎回 8 個を足し直す。「古い値を引いて新しい値を足す」差分更新のほうが速いが、
    // float の丸め誤差が何時間も積もり続ける。125 ms に対して 8 回の加算は十分
    // 安いので、素直に足し直す。
    float sum_p = 0.0f;
    float sum_t = 0.0f;
    for (uint8_t i = 0; i < AVG_N; i++) {
        sum_p += s_buf_p[i];
        sum_t += s_buf_t[i];
    }

    // 補正係数を atomic に取り出し
    float pa, pb, ta, tb;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        pa = SharedMemory.reg.press_coef_a;
        pb = SharedMemory.reg.press_coef_b;
        ta = SharedMemory.reg.temp_coef_a;
        tb = SharedMemory.reg.temp_coef_b;
    }

    *out_p = pa * (sum_p / AVG_N) + pb;
    *out_t = ta * (sum_t / AVG_N) + tb;
    return true;
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
    SYSTEM_Initialize();

    // BMP585 用 TWI1 host の初期化 (MCC の SYSTEM_Initialize には含まれていない)
    TWI1_Initialize();

    // EEPROM 読み込み (new_addr / coef_* が確定する)
    EM_loadEEPROM();

    // I2C スレーブアドレスを反映 (TWI0)
    TWI0.SADDR = SharedMemory.reg.new_addr << 1;

    // ===== OSL 共通レジスタの初期化 ===========================================
    // Device ID = SIGROW.SERNUM の FNV-1a ハッシュ (22-bit fold)。
    // 22-bit に揃えることで BACnet Object Instance としてそのまま使える。
    {
        uint8_t serial[16];
        memcpy(serial, (const void*)&SIGROW.SERNUM0, sizeof(serial));
        SharedMemory.reg.device_id = fnv1a_22(serial, sizeof(serial));
    }
    SharedMemory.reg.addr_key      = 0x00;
    SharedMemory.reg.data_count    = 2;
    SharedMemory.reg.unit_type[VAL_IDX_PRESSURE]    = UNIT_PASCALS;
    SharedMemory.reg.unit_type[VAL_IDX_TEMPERATURE] = UNIT_DEGREES_CELSIUS;
    SharedMemory.reg.status1       = STATUS1_ALL_STALE;  // 平均が埋まるまで stale
    SharedMemory.reg.status2       = 0;
    // meas_count は RAM 上の変数なので、再起動すると必ず 0 に戻る。親機はこの値が
    // 1 秒あたり 8 進むかどうかを見て、黙って再起動していないかを判定する。
    SharedMemory.reg.meas_count    = 0;
    SharedMemory.reg.err_count     = 0;
    for (uint8_t i = 0; i < 8; i++) SharedMemory.reg.value[i] = 0.0f;

    // BMP585 初期化 (失敗しても status1 = STATUS1_ALL_STALE のまま継続し、
    // measure() が 1 秒ごとに初期化し直す)
    BMP585_Initialize();

    // RTC を 1 秒周期から 125 ms 周期へ。PER を縮める前に CNT を戻さないと、
    // CNT > PER のまま 0xFFFF まで回ってしまい、初回だけ 2 秒待つことになる。
    RTC_WriteCounter(0);
    RTC_WritePeriod(RTC_PERIOD_TICK);
    RTC_SetOVFIsrCallback(tickHandler);

    // I2C スレーブ初期化
    I2C_Slave_Init();

    set_sleep_mode(SLEEP_MODE_IDLE);
    sei();

    uint8_t rst_hold_count = 0;
    uint8_t clkhold_ticks  = 0;

    while (1)
    {
        // WDT は SYSTEM_Initialize() 内で 4.1 秒周期に設定されている
        // (mcc_generated_files/system/src/system.c の WDT_Initialize)。ヒューズは
        // PERIOD_OFF_gc だがロックされていないので、この実行時の書き込みが有効になる。
        //
        // ところがこの firmware には wdt_reset() がどこにも無く、全個体が 4.1 秒ごとに
        // 再起動していた。再起動のたびに I2C クライアントが「有効だが割り込みが
        // 止まっていて応答できない」区間を通るため、そこに親機のポーリングが当たると
        // SCL が解放されず、バス上の全ノードが巻き添えになる。
        //
        // データにも出ていた。1 秒ポーリングに対して sq がきっちり 4 回に 1 回止まり、
        // 温湿度センサ 7 台とも重複率 25%。WDT を有効にしていない風速センサ 2 台は
        // 0.0% で、設定の違いがそのまま分かれていた。
        wdt_reset();

        // マスタからの拡張領域 (補正係数) 書き込みリクエスト
        if (I2C_Config_Update_Requested)
        {
            I2C_Config_Update_Requested = false;
            EM_updateEEPROM();
        }

        // 計測タイマー (125 ms): BMP585 計測 + Reset スイッチ判定
        if (g_meas_tick)
        {
            g_meas_tick = false;

            // 計測周期ごとに +1。255 の次は 0 に戻る (uint8 の自然な巻き戻り)。
            // BMP585 の読み取りが失敗しても進める。「MCU が生きて回っているか」だけを
            // 表す信号にしたいので、センサの調子は status1 に任せる。
            SharedMemory.reg.meas_count++;

            // TWI0 クライアントの固着検出。
            //
            // SCL を離す手段は SCMD だけだが、生成ドライバにはそれを出さずに ISR を
            // 抜ける経路が残っている (エラー処理と、イベント判定末尾の
            // "No action required")。そこに入ると SCL を解放せずに停止し、I2C は
            // 2 本を共有しているのでバス上の全ノードが読めなくなる。親機側からは
            // 解けない (2026-08-10 に実測。SDA/SCL とも Low、ホストを 32 回初期化
            // しても復帰せず)。
            //
            // 正常な電文は 3.5 ミリ秒で終わる。125 ms 刻みで 1 秒ぶん CLKHOLD が
            // 立ちっぱなしなら異常と断定してよいので、強制的に取引を終わらせる。
            if (TWI0.SSTATUS & TWI_CLKHOLD_bm) {
                if (++clkhold_ticks >= CLKHOLD_STUCK_TICKS) {
                    // 解放していなかったのが自分だったことを err_count (0x2B) に足して
                    // 親機に知らせる。バス上の全員が同じ症状に見えるので、これ以外に
                    // 犯人を特定する手段がない。
                    if (SharedMemory.reg.err_count < 0xFF) SharedMemory.reg.err_count++;
                    TWI0.SSTATUS = (TWI_COLL_bm | TWI_BUSERR_bm | TWI_APIF_bm | TWI_DIF_bm);
                    TWI0.SCTRLB  = TWI_ACKACT_NACK_gc | TWI_SCMD_COMPTRANS_gc;
                    clkhold_ticks = 0;
                }
            } else {
                clkhold_ticks = 0;
            }

            // Reset スイッチ長押し判定 (RESET_HOLD_SECONDS 秒で発動)
            if (IO_RESET_GetValue() == 0) {
                if (++rst_hold_count >= RESET_HOLD_TICKS) {
                    EM_resetEEPROM();
                    rst_hold_count = 0;
                }
            } else {
                rst_hold_count = 0;
            }

            // BMP585 がひとりで再起動していないかを 1 秒ごとに確かめる。
            // 電源の瞬断などで再起動すると測定が止まり、データレジスタには最後の値が
            // 残ったままになる。読み出しは成功し続けるので、失敗回数では気づけない。
            if ((SharedMemory.reg.meas_count % TICKS_PER_SEC) == 0 && BMP585_NeedsRestart()) {
                restartSensor();
            }

            // BMP585 計測
            float corr_p, corr_t;
            if (measure(&corr_p, &corr_t)) {
                // 親機が POLL ブロック 0x28-0x4B の 36 バイトを読んでいる最中に
                // value[] の float を書き換えると、前半旧値・後半新値の壊れた float が
                // valid として渡る。通信中は公開せず、次の計測周期に回す。計測そのものは
                // 上で済ませてあるので途切れない。
                //
                // ATOMIC_BLOCK だけでは足りない。保証されるのは「1 回の書き込みが 1 回の
                // 割り込みに割られない」ことだけで、守りたい単位は 36 バイトの転送全体、
                // つまり何十回もの割り込みにまたがる範囲だから。
                if (!I2C_Is_Busy) {
                    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                        SharedMemory.reg.value[VAL_IDX_PRESSURE]    = corr_p;
                        SharedMemory.reg.value[VAL_IDX_TEMPERATURE] = corr_t;
                        SharedMemory.reg.status2 = 1;
                    }
                }
                ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                    SharedMemory.reg.status1 = 0x00;
                }
            } else {
                ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                    SharedMemory.reg.status1 = STATUS1_ALL_STALE;
                }
            }
        }

        // BMP585 は NORMAL モードで測定し続けるので、マイコンは IDLE スリープで割り込みを待つ
        sleep_mode();
    }
}

// </editor-fold>
