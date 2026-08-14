 /*
 * AVR128DB32 による温湿度計子機 (PoEM-pod 用 OSL 子機)。
 * SHT4x (Sensirion) を TWI1 マスタで読み取り、共通レジスタ仕様
 * (SHASE 2026 第2報) で TWI0 スレーブとして親機に提供する。
 *
 * value[0] = 温度 [°C]    (BACnet unit 62)
 * value[1] = 相対湿度 [%] (BACnet unit 29)
 *
 * 拡張領域には線形補正係数 (温度・湿度の a, b) を保持し、
 * corrected = a * raw + b の式で計測値を補正する。
 */

// <editor-fold defaultstate="collapsed" desc="include">

#include "mcc_generated_files/system/system.h"
#include "mcc_generated_files/system/pins.h"
#include "mcc_generated_files/timer/rtc.h"
#include "mcc_generated_files/i2c_host/twi1.h"

#include "utility.h"
#include "sht4x.h"
#include "i2c_slave.h"
#include "i2c_shared_data.h"
#include "eeprom_manager.h"

#include <util/atomic.h>
#include <string.h>
#include <avr/sleep.h>
#include <avr/wdt.h>

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="定数">

// SHT4x の I2C アドレス (基板実装は SHT40-AD1B = 0x44)
#define SHT4X_TYPE          SHT4_AD

// Reset スイッチ長押しで EEPROM 初期化 [秒]
#define RESET_HOLD_SECONDS  (3)

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="変数">

// 1秒ごとに立つ計測要求フラグ (RTC OVF で true に)
volatile bool g_one_sec_tick = false;

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="計測処理">

// SHT4x から温湿度を読み取り、補正を適用して返す。
// SharedMemory への公開は行わない (公開タイミングは main loop が制御する)
static bool measure(float *out_t, float *out_h)
{
    float raw_t = 0.0f;
    float raw_h = 0.0f;

    if (!SHT4x_ReadValue(&raw_t, &raw_h, SHT4X_TYPE)) {
        return false;
    }

    // 補正係数を atomic に取り出し
    float ta, tb, ha, hb;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        ta = SharedMemory.reg.temp_coef_a;
        tb = SharedMemory.reg.temp_coef_b;
        ha = SharedMemory.reg.humi_coef_a;
        hb = SharedMemory.reg.humi_coef_b;
    }

    *out_t = ta * raw_t + tb;
    *out_h = ha * raw_h + hb;
    return true;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="main">

// 1sec タイマ用コールバック (RTC OVF, 32768/32768 = 1.0s)
static void secHandler(void)
{
    g_one_sec_tick = true;
}

int main(void)
{
    SYSTEM_Initialize();

    // SHT4x 用 TWI1 host の初期化 (MCC の SYSTEM_Initialize には含まれていない)
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
    SharedMemory.reg.unit_type[0]  = UNIT_DEGREES_CELSIUS;
    SharedMemory.reg.unit_type[1]  = UNIT_PERCENT_RELATIVE_HUMID;
    SharedMemory.reg.status1       = 0;
    SharedMemory.reg.status2       = 0;
    for (uint8_t i = 0; i < 8; i++) SharedMemory.reg.value[i] = 0.0f;

    // SHT4x 初期化 (失敗しても status1 = STATUS1_ALL_STALE で通知して継続)
    if (!SHT4x_Initialize(SHT4X_TYPE)) {
        SharedMemory.reg.status1 = STATUS1_ALL_STALE;
    }

    // RTC OVF (1sec) コールバック登録
    RTC_SetOVFIsrCallback(secHandler);

    // I2C スレーブ初期化
    I2C_Slave_Init();

    set_sleep_mode(SLEEP_MODE_IDLE);
    sei();

    uint8_t rst_hold_count = 0;
    bool    clkhold_held   = false;

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

        // 1秒タイマー: SHT4x 計測 + Reset スイッチ判定
        if (g_one_sec_tick)
        {
            g_one_sec_tick = false;

            // TWI0 クライアントの固着検出。
            //
            // SCL を離す手段は SCMD だけだが、生成ドライバにはそれを出さずに ISR を
            // 抜ける経路が残っている (エラー処理と、イベント判定末尾の
            // "No action required")。そこに入ると SCL を解放せずに停止し、I2C は
            // 2 本を共有しているのでバス上の全ノードが読めなくなる。親機側からは
            // 解けない (2026-08-10 に実測。SDA/SCL とも Low、ホストを 32 回初期化
            // しても復帰せず)。
            //
            // 正常な電文は 3.5 ミリ秒で終わる。1 秒周期を 2 回またいで CLKHOLD が
            // 立ちっぱなしなら異常と断定してよいので、強制的に取引を終わらせる。
            if (TWI0.SSTATUS & TWI_CLKHOLD_bm) {
                if (clkhold_held) {
                    // 解放していなかったのが自分だったことを親機に知らせる (0x2A)。
                    // バス上の全員が同じ症状に見えるので、これ以外に犯人を特定
                    // する手段がない。
                    if (SharedMemory.reg.hold_count < 0xFF) SharedMemory.reg.hold_count++;
                    TWI0.SSTATUS = (TWI_COLL_bm | TWI_BUSERR_bm | TWI_APIF_bm | TWI_DIF_bm);
                    TWI0.SCTRLB  = TWI_ACKACT_NACK_gc | TWI_SCMD_COMPTRANS_gc;
                    clkhold_held = false;
                } else {
                    clkhold_held = true;
                }
            } else {
                clkhold_held = false;
            }

            // Reset スイッチ長押し判定 (1sec 単位、RESET_HOLD_SECONDS で発動)
            if (IO_RESET_GetValue() == 0) {
                if (++rst_hold_count >= RESET_HOLD_SECONDS) {
                    EM_resetEEPROM();
                    rst_hold_count = 0;
                }
            } else {
                rst_hold_count = 0;
            }

            // SHT4x 計測
            float corr_t, corr_h;
            if (measure(&corr_t, &corr_h)) {
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
                        SharedMemory.reg.value[VAL_IDX_TEMPERATURE] = corr_t;
                        SharedMemory.reg.value[VAL_IDX_HUMIDITY]    = corr_h;
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

        // SHT4x は常時起動 (idle 0.4µA) のため IDLE スリープに固定
        sleep_mode();
    }
}

// </editor-fold>
