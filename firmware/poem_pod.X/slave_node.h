/*
 * File:   slave_node.h
 * Author: e.togashi
 *
 * 子機を表す構造体と関連定数の定義。
 * 共通レジスタマップ (SHASE 2026 第2報, 表1) に対応する。
 */

#ifndef SLAVE_NODE_H
#define	SLAVE_NODE_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>

// 親機が同時に管理できる子機の上限。
// 64 ノード × ~60 byte = ~3.8 KB。実測で縮減する可能性あり。
#define MAX_NODES               64

// 1 子機あたりの計測値最大数 (共通領域の制約)
#define NODE_MAX_VALUES         8

// I²C アドレスの走査範囲 (7bit、予約域を除いた一般デバイス領域)
#define I2C_ADDR_SCAN_FIRST     0x08
#define I2C_ADDR_SCAN_LAST      0x77

// 共通レジスタアドレス
#define REG_DEVICE_ID           0x00    // uint32 R
#define REG_ADDR_KEY            0x04    // uint8  W
#define REG_NEW_ADDR            0x05    // uint8  R/W
#define REG_DATA_COUNT          0x06    // uint8  R
#define REG_UNIT_TYPE           0x08    // uint16[8] R
#define REG_STATUS1             0x18    // uint8  R    (異常フラグ bitmask)
#define REG_STATUS2             0x19    // uint8  R/W  (0=未了, 1=更新完了)
#define REG_VALUE               0x1C    // float[8] R  (LE)
#define REG_EXTENSION           0x3C    // 各子機固有 (親機は触らない)

// ステータス→バリュー部の一括読み取り長 (0x18~0x3B = 36 byte)
#define POLL_BLOCK_OFFSET       REG_STATUS1
#define POLL_BLOCK_LEN          36

// 共通領域の固定情報 (DeviceID + 予約 + DataCount + 予約 + UnitType[8]) = 0x00~0x17 = 24 byte
#define INFO_BLOCK_OFFSET       REG_DEVICE_ID
#define INFO_BLOCK_LEN          24

// シーケンス番号の特殊値: 親機がまだ一度も新値を観測していないことを示す
#define SEQ_INITIAL             255

// Status1 のビット規約 (per-value bitmask):
//   bit i (i < data_count) = value[i] が stale / 異常 (1=信頼できない)
//   全計測値が信頼できないときは 0xFF を立てる慣例
//   親機が子機を読めなかった場合も 0xFF として publish する
#define STATUS1_ALL_STALE       0xFF

// 親機側 RAM に保持する子機状態
typedef struct {
    uint8_t  i2c_addr;                          // 7bit
    uint32_t device_id;                         // FNV-1a (子機側で生成、親はそのまま受け取る)
    uint8_t  data_count;                        // 有効な計測値の個数 (≤ NODE_MAX_VALUES)
    uint16_t unit_type[NODE_MAX_VALUES];        // BACnet Engineering Units
    uint8_t  status1;                           // 直近に読んだ Status1 (異常 bitmask)
    uint8_t  status2;                           // 直近に読んだ Status2 (0/1)
    uint8_t  seq;                               // 親機管理シーケンス番号 (0~254、255=未計測)
    float    value[NODE_MAX_VALUES];            // 直近の計測値
} slave_node_t;


#ifdef	__cplusplus
}
#endif

#endif	/* SLAVE_NODE_H */
