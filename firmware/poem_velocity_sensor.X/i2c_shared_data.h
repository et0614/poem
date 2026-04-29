/*
 * File:   i2c_shared_data.h
 * Author: E. Togashi
 *
 * SHASE 2026 第2報 共通レジスタ仕様に準拠した子機 (風速センサ) のレジスタマップ。
 * 0x00-0x3B が共通領域、0x3C 以降が拡張領域。
 */

#ifndef I2C_SHARED_DATA_H
#define	I2C_SHARED_DATA_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

// 構造体のパッキング (隙間埋めを禁止してバイトオフセットを仕様に揃える)
#pragma pack(push, 1)

typedef struct {
    // ===== 共通領域 (0x00-0x3B) =============================================
    uint32_t device_id;        // 0x00 R   : FNV-1a(SIGROW.SERNUM) ハッシュ
    uint8_t  addr_key;         // 0x04 W   : アドレス変更鍵 (STOP で自動クリア)
    uint8_t  new_addr;         // 0x05 R/W : 新しい I2C アドレス
    uint8_t  data_count;       // 0x06 R   : 有効計測値の数 (=2)
    uint8_t  reserved_07;      // 0x07     : 予約 (4byte alignment)
    uint16_t unit_type[8];     // 0x08 R   : BACnet 単位コード (LE)
    uint8_t  status1;          // 0x18 R   : 状態フラグ
    uint8_t  status2;          // 0x19 R/W : 計測値更新フラグ (0=未了, 1=更新)
    uint8_t  reserved_1A[2];   // 0x1A     : 予約 (4byte alignment)
    float    value[8];         // 0x1C R   : 計測値 (LE float)

    // ===== 拡張領域 (0x3C-) =================================================
    uint8_t  enable;           // 0x3C R/W : 風速計回路の起動フラグ
    uint8_t  filter_n;         // 0x3D R/W : 平滑化フィルタ係数 (0~20)
    float    coefficientA[5];  // 0x3E R/W : 風速換算係数 A
    float    coefficientB[5];  // 0x52 R/W : 風速換算係数 B
} SensorData_t;

// 共用体
typedef union {
    SensorData_t reg;
    uint8_t      bytes[sizeof(SensorData_t)];
} I2C_Map_t;

#pragma pack(pop)

// 実体は i2c_slave.c で定義
extern volatile I2C_Map_t SharedMemory;

// ===== 共通レジスタアドレス (親機 slave_node.h と同期) =====================
#define REG_DEVICE_ID     0x00
#define REG_ADDR_KEY      0x04
#define REG_NEW_ADDR      0x05
#define REG_DATA_COUNT    0x06
#define REG_UNIT_TYPE     0x08
#define REG_STATUS1       0x18
#define REG_STATUS2       0x19
#define REG_VALUE         0x1C
#define REG_EXTENSION     0x3C

// ===== アドレス変更鍵 ======================================================
#define ADDR_KEY_UNLOCK   0xA5

// ===== Status1 ビット定義 ==================================================
#define STATUS1_HEATING_BIT    (0)
#define STATUS1_HEATING_MASK   (1 << STATUS1_HEATING_BIT)

// ===== 計測値インデックス ==================================================
#define VAL_IDX_VELOCITY  (0)  // value[0] = 風速 [m/s]
#define VAL_IDX_VOLTAGE   (1)  // value[1] = 風速計回路の生電圧 [V]

// ===== BACnet Engineering Units (ASHRAE 135) ==============================
#define UNIT_VOLTS              (5)
#define UNIT_METERS_PER_SECOND  (74)

#ifdef	__cplusplus
}
#endif

#endif	/* I2C_SHARED_DATA_H */
