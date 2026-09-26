#include "mcc_generated_files/system/clock.h" //F_CPUの設定
#include "mcc_generated_files/timer/delay.h"
#include "bmp585.h"
#include "i2c_master.h"

// ===== レジスタ ============================================================
#define REG_CHIP_ID         0x01
#define REG_TEMP_DATA_XLSB  0x1D    // 0x1D-0x1F 温度、0x20-0x22 気圧 (XLSB, LSB, MSB の順)
#define REG_INT_STATUS      0x27    // 読むとクリアされる
#define REG_STATUS          0x28
#define REG_OSR_CONFIG      0x36
#define REG_ODR_CONFIG      0x37
#define REG_OSR_EFF         0x38
#define REG_CMD             0x7E

#define CHIP_ID_BMP585      0x51
#define CMD_SOFT_RESET      0xB6

#define INT_STATUS_POR      (1 << 4)
#define STATUS_NVM_RDY      (1 << 1)
#define STATUS_NVM_ERR      (1 << 2)
#define OSR_EFF_ODR_VALID   (1 << 7)

// ===== 測定設定 ============================================================
// 気圧 128 倍、温度 8 倍のオーバーサンプリング。この組み合わせで許される
// 最大 ODR はちょうど 10 Hz (データシート Table 7)。
#define OSR_P_128X          0x7
#define OSR_T_8X            0x3
#define OSR_CONFIG_VALUE    ((1 << 6) | (OSR_P_128X << 3) | OSR_T_8X)   // press_en = 1

// ODR 10 Hz。子機は 125 ms (8 Hz) ごとに読むので、毎回新しい値が取れる。
// deep_dis = 1 で DEEP STANDBY を禁止する (ODR ≥ 5 Hz なので元々入らないが明示)。
#define ODR_10HZ            0x17
#define PWR_MODE_STANDBY    0x0
#define PWR_MODE_NORMAL     0x1
#define ODR_CONFIG_BASE     ((1 << 7) | (ODR_10HZ << 2))

// 測定範囲 30〜125 kPa。外れた値は通信化けか、再起動直後の空のデータレジスタとみなす。
#define PRESS_MIN_PA        (30000.0f)
#define PRESS_MAX_PA        (125000.0f)

static bool writeReg(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return I2C_Write(BMP585_ADDR, buf, 2);
}

static bool readRegs(uint8_t reg, uint8_t *buf, uint8_t len)
{
    return I2C_WriteRead(BMP585_ADDR, &reg, 1, buf, len);
}

bool BMP585_Initialize(void)
{
    // ソフトリセット。I2C で書くとセンサは ACK を返さない仕様なので、
    // I2C_Write の戻り値 (NACK で false) は見ない。リセット完了まで最大 2 ms。
    writeReg(REG_CMD, CMD_SOFT_RESET);
    DELAY_milliseconds(3);

    // 電源投入後の確認手順 (データシート 4.3.9)
    uint8_t v;
    if (!readRegs(REG_CHIP_ID, &v, 1) || v != CHIP_ID_BMP585) return false;
    if (!readRegs(REG_STATUS, &v, 1)) return false;
    if (!(v & STATUS_NVM_RDY) || (v & STATUS_NVM_ERR)) return false;
    // 読むと por がクリアされる。以後 por が立てば「センサが自分で再起動した」の意味になる。
    // リセット直後なら por = 1 のはずだが、確認の役には立たないので値は問わない。
    if (!readRegs(REG_INT_STATUS, &v, 1)) return false;

    // 設定は STANDBY で書き、最後に NORMAL へ移す。
    // モードの切り替えは必ず STANDBY を経由する決まり (データシート 4.3)。
    if (!writeReg(REG_OSR_CONFIG, OSR_CONFIG_VALUE)) return false;
    if (!writeReg(REG_ODR_CONFIG, ODR_CONFIG_BASE | PWR_MODE_STANDBY)) return false;
    DELAY_milliseconds(3);  // STANDBY への遷移 最大 2.5 ms

    // OSR と ODR の組み合わせが無効だと、センサは黙って既定の OSR で動く。
    // 設定ミスに気づけるよう、ここで失敗にする。
    if (!readRegs(REG_OSR_EFF, &v, 1) || !(v & OSR_EFF_ODR_VALID)) return false;

    if (!writeReg(REG_ODR_CONFIG, ODR_CONFIG_BASE | PWR_MODE_NORMAL)) return false;
    return true;
}

bool BMP585_ReadValue(float *pressValue, float *tempValue)
{
    *pressValue = -99;
    *tempValue = -99;

    // 温度と気圧を 1 回のバースト読み出しで取る。NORMAL モードは読み出しと測定が
    // 同期していないので、まとめて読んだときだけシャドウイングで同じ測定の組が
    // 保証される (データシート 4.5.1)。
    uint8_t b[6];
    if (!readRegs(REG_TEMP_DATA_XLSB, b, 6)) return false;

    // 温度は 24 bit 符号付き、2^16 で割って °C
    int32_t raw_t = ((int32_t)b[2] << 16) | ((int32_t)b[1] << 8) | b[0];
    if (raw_t & 0x800000L) raw_t -= 0x1000000L;

    // 気圧は 24 bit 符号なし、2^6 で割って Pa
    uint32_t raw_p = ((uint32_t)b[5] << 16) | ((uint32_t)b[4] << 8) | b[3];

    float p = (float)raw_p / 64.0f;
    if (p < PRESS_MIN_PA || p > PRESS_MAX_PA) return false;

    *pressValue = p;
    *tempValue  = (float)raw_t / 65536.0f;
    return true;
}

bool BMP585_NeedsRestart(void)
{
    uint8_t v;
    if (!readRegs(REG_INT_STATUS, &v, 1)) return true;
    return (v & INT_STATUS_POR) != 0;
}
