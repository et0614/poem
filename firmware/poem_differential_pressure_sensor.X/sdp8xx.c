#include "mcc_generated_files/system/clock.h" //F_CPUの設定
#include "mcc_generated_files/timer/delay.h"
#include "sdp8xx.h"
#include "i2c_master.h"
#include "utility.h"

// 連続測定: 差圧で温度補償、読み出しまで平均 (average till read)
#define CMD_START_CONT_DP_AVG   0x3615
// 連続測定の停止。停止後 500 µs でコマンドを受け付けられるようになる
#define CMD_STOP_CONT           0x3FF9

// 温度の換算係数 [1/°C] (データシート 6.5.1、全機種共通)
#define TEMP_SCALE_FACTOR       (200.0f)

// 直近に読めた差圧の換算係数 (0 = まだ読めていない)。機種の判別に使う。
static uint16_t s_last_sf = 0;

static bool sendCommand(uint16_t cmd)
{
    const uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)cmd };
    return I2C_Write(SDP8XX_ADDR, buf, 2);
}

bool SDP8xx_Initialize(void)
{
    // マイコンだけが再起動した (WDT など) 場合、センサは連続測定を続けたままで、
    // 停止コマンド以外を受け付けない。まず止めてから開始し直す。
    // 停止中のセンサにも停止コマンドは通るので、状態によらず同じ手順でよい。
    sendCommand(CMD_STOP_CONT);
    DELAY_milliseconds(1);

    if (!sendCommand(CMD_START_CONT_DP_AVG)) return false;

    // 初回の結果が出るまで 8 ms。その後 12 ms は数 % の誤差が出うるが、
    // 起動直後は移動平均が埋まるまで (1 秒) 公開しないので問題にならない。
    DELAY_milliseconds(10);
    return true;
}

bool SDP8xx_ReadValue(float *dpValue, float *tempValue)
{
    *dpValue = -99;
    *tempValue = -99;

    // 連続測定中はコマンド不要。読み出しヘッダだけで結果が返る。
    // (DP MSB, DP LSB, CRC, T MSB, T LSB, CRC, SF MSB, SF LSB, CRC)
    uint8_t buffer[9];
    if (!I2C_Read(SDP8XX_ADDR, buffer, 9)) return false;

    // CRCチェック
    if (calc_crc8(&buffer[0], 2) != buffer[2]) return false;
    if (calc_crc8(&buffer[3], 2) != buffer[5]) return false;
    if (calc_crc8(&buffer[6], 2) != buffer[8]) return false;

    int16_t  raw_dp = (int16_t)(((uint16_t)buffer[0] << 8) | buffer[1]);
    int16_t  raw_t  = (int16_t)(((uint16_t)buffer[3] << 8) | buffer[4]);
    uint16_t sf     = ((uint16_t)buffer[6] << 8) | buffer[7];
    if (sf == 0) return false;
    s_last_sf = sf;

    *dpValue   = (float)raw_dp / (float)sf;
    *tempValue = (float)raw_t / TEMP_SCALE_FACTOR;
    return true;
}

uint16_t SDP8xx_LastScaleFactor(void)
{
    return s_last_sf;
}
