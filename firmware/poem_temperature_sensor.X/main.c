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

// SHT4x から温湿度を読み取り、補正を適用して value[] に書き込む
static bool measureAndPublish(void)
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

    float corr_t = ta * raw_t + tb;
    float corr_h = ha * raw_h + hb;

    // 計測値の書き込み + 更新フラグ
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        SharedMemory.reg.value[VAL_IDX_TEMPERATURE] = corr_t;
        SharedMemory.reg.value[VAL_IDX_HUMIDITY]    = corr_h;
        SharedMemory.reg.status2 = 1;
    }
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
    // Device ID = SIGROW.SERNUM の FNV-1a ハッシュ
    {
        uint8_t serial[16];
        memcpy(serial, (const void*)&SIGROW.SERNUM0, sizeof(serial));
        SharedMemory.reg.device_id = fnv1a_32(serial, sizeof(serial));
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

    while (1)
    {
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
            if (measureAndPublish()) {
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
