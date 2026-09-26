#include "mcc_generated_files/nvm/nvm.h"
#include "eeprom_manager.h"
#include "i2c_shared_data.h"

#include <string.h>
#include <util/atomic.h>

// AVR128DB32 EEPROM 開始アドレス
#define EEPROM_BASE_ADDR  0x1400

// EEPROM レイアウト (native LE で保存)
// 温湿度センサと同じ並び (係数は value[0] の a, b → value[1] の a, b)。
typedef struct {
    uint8_t init_flag;                  // 'T' なら初期化済み
    uint8_t i2c_addr;                   // I2C アドレス
    uint8_t reserved[2];                // 4byte alignment
    float   press_coef_a;
    float   press_coef_b;
    float   temp_coef_a;
    float   temp_coef_b;
    char    name[NODE_NAME_LEN];        // 装置ラベル (NUL 終端)
} EepromMap;

#define ADDR_INIT_FLAG    (EEPROM_BASE_ADDR + offsetof(EepromMap, init_flag))
#define ADDR_I2C_ADDR     (EEPROM_BASE_ADDR + offsetof(EepromMap, i2c_addr))
#define ADDR_PRESS_COEF_A (EEPROM_BASE_ADDR + offsetof(EepromMap, press_coef_a))
#define ADDR_PRESS_COEF_B (EEPROM_BASE_ADDR + offsetof(EepromMap, press_coef_b))
#define ADDR_TEMP_COEF_A  (EEPROM_BASE_ADDR + offsetof(EepromMap, temp_coef_a))
#define ADDR_TEMP_COEF_B  (EEPROM_BASE_ADDR + offsetof(EepromMap, temp_coef_b))
#define ADDR_NAME         (EEPROM_BASE_ADDR + offsetof(EepromMap, name))

// デフォルト値
#define DEFAULT_I2C_ADDRESS    (0x10)
#define DEFAULT_COEF_A         (1.0f)
#define DEFAULT_COEF_B         (0.0f)

// 直近書き込み値 (差分書き込み判定用)
static uint8_t last_i2c_address;
static float   last_press_coef_a;
static float   last_press_coef_b;
static float   last_temp_coef_a;
static float   last_temp_coef_b;
static char    last_name[NODE_NAME_LEN];

// <editor-fold defaultstate="collapsed" desc="EEPROMブロック読み書き">

static void write_eep_block(const void* src, uint16_t dst_addr, size_t size)
{
    const uint8_t* pSrc = (const uint8_t*)src;
    for (size_t i = 0; i < size; i++) {
        while (EEPROM_IsBusy());
        if (EEPROM_Read(dst_addr + i) != pSrc[i]) {
            while (EEPROM_IsBusy());
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                EEPROM_Write(dst_addr + i, pSrc[i]);
            }
        }
    }
    while (EEPROM_IsBusy());
}

static void read_eep_block(void* dst, uint16_t src_addr, size_t size)
{
    uint8_t* pDst = (uint8_t*)dst;
    for (size_t i = 0; i < size; i++) {
        pDst[i] = EEPROM_Read(src_addr + i);
    }
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="公開関数">

void EM_loadEEPROM(void)
{
    if (EEPROM_Read(ADDR_INIT_FLAG) != 'T')
    {
        EM_resetEEPROM();
        while (EEPROM_IsBusy());
        EEPROM_Write(ADDR_INIT_FLAG, 'T');
    }

    // I2C アドレス
    while (EEPROM_IsBusy());
    SharedMemory.reg.new_addr = EEPROM_Read(ADDR_I2C_ADDR);
    last_i2c_address = SharedMemory.reg.new_addr;

    // 気圧補正係数
    read_eep_block(&last_press_coef_a, ADDR_PRESS_COEF_A, sizeof(float));
    read_eep_block(&last_press_coef_b, ADDR_PRESS_COEF_B, sizeof(float));
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        SharedMemory.reg.press_coef_a = last_press_coef_a;
        SharedMemory.reg.press_coef_b = last_press_coef_b;
    }

    // 温度補正係数
    read_eep_block(&last_temp_coef_a, ADDR_TEMP_COEF_A, sizeof(float));
    read_eep_block(&last_temp_coef_b, ADDR_TEMP_COEF_B, sizeof(float));
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        SharedMemory.reg.temp_coef_a = last_temp_coef_a;
        SharedMemory.reg.temp_coef_b = last_temp_coef_b;
    }

    // 装置ラベル (未初期化 = 0xFF なら空文字にする)
    read_eep_block(last_name, ADDR_NAME, NODE_NAME_LEN);
    if ((unsigned char)last_name[0] == 0xFF) {
        memset(last_name, 0, NODE_NAME_LEN);
    }
    last_name[NODE_NAME_LEN - 1] = '\0';
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        memcpy((void*)SharedMemory.reg.name, last_name, NODE_NAME_LEN);
    }
}

void EM_updateEEPROM(void)
{
    // I2C アドレス
    if (last_i2c_address != SharedMemory.reg.new_addr)
    {
        last_i2c_address = SharedMemory.reg.new_addr;
        while (EEPROM_IsBusy());
        EEPROM_Write(ADDR_I2C_ADDR, SharedMemory.reg.new_addr);
    }

    // 気圧・温度補正係数 (現在値を atomic に取り出して比較)
    float currentVals[4];
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        currentVals[0] = SharedMemory.reg.press_coef_a;
        currentVals[1] = SharedMemory.reg.press_coef_b;
        currentVals[2] = SharedMemory.reg.temp_coef_a;
        currentVals[3] = SharedMemory.reg.temp_coef_b;
    }

    if (memcmp(&last_press_coef_a, &currentVals[0], sizeof(float)) != 0) {
        last_press_coef_a = currentVals[0];
        write_eep_block(&last_press_coef_a, ADDR_PRESS_COEF_A, sizeof(float));
    }
    if (memcmp(&last_press_coef_b, &currentVals[1], sizeof(float)) != 0) {
        last_press_coef_b = currentVals[1];
        write_eep_block(&last_press_coef_b, ADDR_PRESS_COEF_B, sizeof(float));
    }
    if (memcmp(&last_temp_coef_a, &currentVals[2], sizeof(float)) != 0) {
        last_temp_coef_a = currentVals[2];
        write_eep_block(&last_temp_coef_a, ADDR_TEMP_COEF_A, sizeof(float));
    }
    if (memcmp(&last_temp_coef_b, &currentVals[3], sizeof(float)) != 0) {
        last_temp_coef_b = currentVals[3];
        write_eep_block(&last_temp_coef_b, ADDR_TEMP_COEF_B, sizeof(float));
    }

    // 装置ラベル
    char currentName[NODE_NAME_LEN];
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        memcpy(currentName, (const void*)SharedMemory.reg.name, NODE_NAME_LEN);
    }
    if (memcmp(last_name, currentName, NODE_NAME_LEN) != 0) {
        memcpy(last_name, currentName, NODE_NAME_LEN);
        write_eep_block(last_name, ADDR_NAME, NODE_NAME_LEN);
    }
}

void EM_resetEEPROM(void)
{
    // I2C アドレス
    while (EEPROM_IsBusy());
    EEPROM_Write(ADDR_I2C_ADDR, DEFAULT_I2C_ADDRESS);

    // 補正係数 (恒等変換: a=1.0, b=0.0)
    const float def_a = DEFAULT_COEF_A;
    const float def_b = DEFAULT_COEF_B;
    write_eep_block(&def_a, ADDR_PRESS_COEF_A, sizeof(float));
    write_eep_block(&def_b, ADDR_PRESS_COEF_B, sizeof(float));
    write_eep_block(&def_a, ADDR_TEMP_COEF_A, sizeof(float));
    write_eep_block(&def_b, ADDR_TEMP_COEF_B, sizeof(float));

    // 装置ラベル: 空文字 (NUL 16 byte)
    char empty_name[NODE_NAME_LEN] = {0};
    write_eep_block(empty_name, ADDR_NAME, NODE_NAME_LEN);
}

// </editor-fold>
