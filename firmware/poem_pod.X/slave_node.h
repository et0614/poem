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
//
// 静的情報 (INFO BLOCK, 走査時に 1 回読む) と動的情報 (POLL BLOCK, 毎周期読む) を
// 連続領域に分離。Reserved はアラインメント (uint16 / float の偶/4byte 境界) 用。
#define REG_DEVICE_ID           0x00    // uint32 R    : FNV-1a(SIGROW.SERNUM)
#define REG_ADDR_KEY            0x04    // uint8  W    : アドレス変更鍵
#define REG_NEW_ADDR            0x05    // uint8  R/W  : 新 I2C アドレス
#define ADDR_KEY_UNLOCK         0xA5    // REG_ADDR_KEY に書き込むとアドレス変更解錠 (STOP で自動クリア)
#define REG_DATA_COUNT          0x06    // uint8  R    : 有効計測値数
                                         // 0x07         : reserved (uint16 整列用)
#define REG_UNIT_TYPE           0x08    // uint16[8] R : BACnet 単位コード (LE)
#define REG_NAME                0x18    // char[16] R/W: 装置ラベル (NUL 終端、EEPROM 永続)
#define REG_STATUS1             0x28    // uint8  R    : 異常フラグ bitmask
#define REG_STATUS2             0x29    // uint8  R/W  : 0=未了, 1=更新完了
// 0x2A-0x2B は元は予約 (float 整列用) だった 2 バイト。POLL BLOCK の内側にあり毎周期
// 読んで捨てていたので、通信を増やさずに子機の自己診断を運ぶために割り当てた。
// I2C は 2 本を全員で共有しており、1 台が SCL を解放しないとバス上の全ノードが止まる。
// 症状は全台同じに見えるため、どの個体が原因かは子機自身に数えさせるしかない。
// 未対応の子機は 0 のままなので、混在しても誤検出しない。
#define REG_HOLD_COUNT          0x2A    // uint8  R    : SCL 固着を自力解除した回数
#define REG_ERR_COUNT           0x2B    // uint8  R    : COLL/BUSERR を検出した回数
#define REG_VALUE               0x2C    // float[8] R  : 計測値 (LE)
#define REG_EXTENSION           0x4C    // 各子機固有 (親機は触らない)

// ステータス→バリュー部の一括読み取り長 (0x28~0x4B = 36 byte)
#define POLL_BLOCK_OFFSET       REG_STATUS1
#define POLL_BLOCK_LEN          36

// 共通領域の固定情報 (DeviceID～UnitType + Name) = 0x00~0x27 = 40 byte
#define INFO_BLOCK_OFFSET       REG_DEVICE_ID
#define INFO_BLOCK_LEN          40

// シーケンス番号の特殊値: 親機がまだ一度も新値を観測していないことを示す
#define SEQ_INITIAL             255

// Status1 のビット規約 (per-value bitmask):
//   bit i (i < data_count) = value[i] が stale / 異常 (1=信頼できない)
//   全計測値が信頼できないときは 0xFF を立てる慣例
//   親機が子機を読めなかった場合も 0xFF として publish する
#define STATUS1_ALL_STALE       0xFF

// 子機名フィールドの長さ (15 文字 + NUL)。共通レジスタ仕様 (REG_NAME = 0x18) に
// 16 byte 確保。子機側 EEPROM に永続化される。
#define NODE_NAME_LEN           16

// 親機側 RAM に保持する子機状態
typedef struct {
    uint8_t  i2c_addr;                          // 7bit
    uint32_t device_id;                         // FNV-1a 22-bit (BACnet Object Instance, 0..0x3FFFFE)
    uint8_t  data_count;                        // 有効な計測値の個数 (≤ NODE_MAX_VALUES)
    uint16_t unit_type[NODE_MAX_VALUES];        // BACnet Engineering Units
    uint8_t  status1;                           // 直近に読んだ Status1 (異常 bitmask)
    uint8_t  status2;                           // 直近に読んだ Status2 (0/1)
    uint8_t  hold_count;                        // 直近に読んだ 0x2A (子機の固着自力解除回数)
    uint8_t  err_count;                         // 直近に読んだ 0x2B (子機の COLL/BUSERR 回数)
    uint8_t  seq;                               // 親機管理シーケンス番号 (0~254、255=未計測)
    float    value[NODE_MAX_VALUES];            // 直近の計測値
    char     name[NODE_NAME_LEN];               // Web 設定画面の表示用ラベル (5文字 + NUL)
} slave_node_t;


#ifdef	__cplusplus
}
#endif

#endif	/* SLAVE_NODE_H */
