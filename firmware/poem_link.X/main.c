/*
 * PoEM Link: USB-MIDI ⇔ I2C ブリッジ (AVR32DU20)。
 *
 * iPhone / Android / PC と Qwiic (I2C) 機器を USB Type-C でつなぐ。USB-MIDI の
 * SysEx に独自コマンドを載せて I2C の読み書きを中継する (E-Sensor と同じ方式)。
 * ファームウェアは I2C の低レベルの処理だけを行い、機器の中身は解釈しない。
 * 仕様は「PoEM_Link_通信仕様.md」。
 *
 * 構成:
 *   i2c_host.c  L0〜L1  TWI0 の転送、タイムアウト、バスの確認、固着からの復帰
 *   sysex.c     L2      USB-MIDI のパケット ⇔ SysEx、7bit 符号化
 *   bridge.c    L2      コマンドの振り分けと応答の組み立て
 *   led.c               状態表示
 */

#include "mcc_generated_files/system/system.h"
#include "mcc_generated_files/timer/tca0.h"

#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/sleep.h>
#include <util/atomic.h>

#include "tinyusb/tusb.h"
#include "systime.h"
#include "i2c_host.h"
#include "sysex.h"
#include "led.h"

// 1 ms 刻みの時刻。TCA0 のオーバーフロー割り込み (12 MHz / 12000) で進める。
static volatile uint32_t system_millis = 0;

static void msecHandler(void)
{
    system_millis++;
}

uint32_t SYSTIME_Millis(void)
{
    uint32_t now;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        now = system_millis;
    }
    return now;
}

// TinyUSB が参照する時刻
uint32_t tusb_time_millis_api(void)
{
    return SYSTIME_Millis();
}

int main(void)
{
    // MCC の初期化。クロック (12 MHz、USB の SOF で自動補正)、ピン、TWI0、TCA0、
    // WDT (4.1 秒) を設定する。VUSB は VDD と直結しているので内蔵の USB 用
    // レギュレータは使わない (VUSBCTRL の初期値 0 のまま)。
    SYSTEM_Initialize();

    TCA0_OverflowCallbackRegister(msecHandler);
    I2CH_Init();

    tusb_init();
    tud_connect();      // USB のホストに接続を知らせる

    sei();

    // IDLE スリープ: CPU だけを止め、USB・TCA0・TWI は動かし続ける。
    // 遅くとも 1 ms ごとに TCA0 で起きるので、処理を取りこぼさない。
    // STANDBY 以下は USB のクロックが止まり接続が切れるので使えない。
    set_sleep_mode(SLEEP_MODE_IDLE);

    while (1) {
        wdt_reset();
        tud_task();         // USB スタック
        SYSEX_Task();       // 届いた SysEx を処理して応答する
        LED_Task();         // 状態表示
        sleep_mode();
    }
}
