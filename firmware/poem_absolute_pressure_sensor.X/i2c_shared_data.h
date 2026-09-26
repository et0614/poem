/*
 * File:   i2c_shared_data.h
 * Author: E. Togashi
 *
 * SHASE 2026 第2報 共通レジスタ仕様に準拠した子機 (絶対圧センサ) のレジスタマップ。
 * 0x00-0x4B が共通領域、0x4C 以降が拡張領域 (線形補正係数 y = a*x + b)。
 *
 * 共通領域は 静的情報 (INFO BLOCK 0x00-0x27) と 動的情報 (POLL BLOCK 0x28-0x4B)
 * に分離。親機は走査時に INFO を 1 回、毎周期 POLL を 1 回読む。
 */

#ifndef I2C_SHARED_DATA_H
#define	I2C_SHARED_DATA_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

// 装置ラベル長 (15 文字 + NUL)
#define NODE_NAME_LEN     16

// 構造体のパッキング (隙間埋めを禁止してバイトオフセットを仕様に揃える)
#pragma pack(push, 1)

typedef struct {
    // ===== INFO BLOCK (0x00-0x27, 走査時に 1 回読む) =========================
    uint32_t device_id;             // 0x00 R   : FNV-1a 22-bit (BACnet Object Instance 互換, 0..0x3FFFFE)
    uint8_t  addr_key;              // 0x04 W   : アドレス変更鍵 (STOP で自動クリア)
    uint8_t  new_addr;              // 0x05 R/W : 新しい I2C アドレス
    uint8_t  data_count;            // 0x06 R   : 有効計測値の数 (=2)
    uint8_t  reserved_07;           // 0x07     : 予約 (uint16 alignment)
    uint16_t unit_type[8];          // 0x08 R   : BACnet 単位コード (LE)
    char     name[NODE_NAME_LEN];   // 0x18 R/W : 装置ラベル (NUL 終端、EEPROM 永続)

    // ===== POLL BLOCK (0x28-0x4B, 毎周期読む) ================================
    uint8_t  status1;               // 0x28 R   : 状態フラグ
    uint8_t  status2;               // 0x29 R/W : 計測値更新フラグ (0=未了, 1=更新)
    // 0x2A-0x2B は元は予約 (float 整列用) だった 2 バイト。POLL BLOCK の内側にあり
    // 親機が毎周期読んで捨てていたので、通信を増やさずに自己診断を運べる。
    //
    // meas_count は自分が数えた計測回数。RAM 上なので再起動すると 0 に戻る。
    // 親機はこれを MQTT の sq に載せる。125 ms 周期なので 1 秒あたり 8 進むはずで、
    // 進み方が崩れれば「この個体が黙って再起動した / 止まった」と分かる。
    //
    // err_count は I2C の異常回数。COLL/BUSERR の検出と、CLKHOLD が 1 秒続いて
    // 自力で取引を打ち切った場合の両方を数える。I2C は 2 本を全員で共有しているため
    // 1 台が SCL を解放しないとバス全体が止まり、親機からは全員が同じ症状に見える。
    // どの個体が原因かを特定する手段が他にないので、ここに出す。
    uint8_t  meas_count;            // 0x2A R   : 計測ごとに +1。255 の次は 0。起動時は 0
    uint8_t  err_count;             // 0x2B R   : I2C 異常を検出した回数 (255 で飽和)
    float    value[8];              // 0x2C R   : 計測値 (LE float)

    // ===== 拡張領域 (0x4C-) ==================================================
    // 線形補正係数: corrected = a * raw + b
    // 並びは温湿度センサと同じ「value[0] の a, b → value[1] の a, b」。
    float    press_coef_a;          // 0x4C R/W : 気圧補正係数 a (default 1.0)
    float    press_coef_b;          // 0x50 R/W : 気圧補正係数 b [Pa] (default 0.0)
    float    temp_coef_a;           // 0x54 R/W : 温度補正係数 a (default 1.0)
    float    temp_coef_b;           // 0x58 R/W : 温度補正係数 b (default 0.0)
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
#define REG_NAME          0x18
#define REG_STATUS1       0x28
#define REG_STATUS2       0x29
#define REG_VALUE         0x2C
#define REG_EXTENSION     0x4C

// ===== アドレス変更鍵 ======================================================
#define ADDR_KEY_UNLOCK   0xA5

// ===== Status1 ビット規約 =================================================
// per-value bitmask: bit i (i < data_count) = value[i] が stale / 異常
// 全計測値が信頼できないときは 0xFF を立てる慣例
// (例: BMP585 通信失敗、親機が子機を読めなかった場合)
#define STATUS1_ALL_STALE         (0xFF)

// ===== 計測値インデックス ==================================================
#define VAL_IDX_PRESSURE     (0)  // value[0] = 絶対圧 [Pa]
#define VAL_IDX_TEMPERATURE  (1)  // value[1] = センサ温度 [°C] (基板の温度。気温ではない)

// ===== BACnet Engineering Units (ASHRAE 135) ==============================
#define UNIT_PASCALS                 (53)
#define UNIT_DEGREES_CELSIUS         (62)

#ifdef	__cplusplus
}
#endif

#endif	/* I2C_SHARED_DATA_H */
