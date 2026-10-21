#include "led.h"
#include "i2c_host.h"
#include "systime.h"
#include "tinyusb/tusb.h"
#include "mcc_generated_files/system/pins.h"

#include <stdbool.h>

// 転送ごとに消える時間 [ms]。目で見える最短に近い長さ。
#define ACTIVITY_OFF_MS     30

static uint32_t s_activity_until = 0;
static uint32_t s_identify_until = 0;
static bool     s_identify       = false;

void LED_Activity(void)
{
    s_activity_until = SYSTIME_Millis() + ACTIVITY_OFF_MS;
}

void LED_Identify(uint8_t seconds)
{
    s_identify       = (seconds > 0);
    s_identify_until = SYSTIME_Millis() + (uint32_t)seconds * 1000UL;
}

// 周期 period_ms の点滅で、前半を点灯とする
static bool blink(uint32_t now, uint16_t period_ms)
{
    return (now % period_ms) < (period_ms / 2);
}

void LED_Task(void)
{
    uint32_t now = SYSTIME_Millis();
    bool on;

    if (s_identify && SYSTIME_IsPast(s_identify_until)) s_identify = false;

    if (s_identify) {
        on = blink(now, 500);                   // 識別: 2 Hz
    } else if (I2CH_IsStuck()) {
        on = blink(now, 200);                   // バス固着: 5 Hz
    } else if (!tud_mounted()) {
        on = blink(now, 1000);                  // USB 未接続: 1 Hz
    } else {
        on = SYSTIME_IsPast(s_activity_until);  // 接続済み: 点灯 (転送のたびに短く消える)
    }

    if (on) LED_SetHigh();
    else    LED_SetLow();
}
