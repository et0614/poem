/*
 * File:   slave_node.h
 * Author: e.togashi
 *
 * 子機を表す構造体と関連定数の定義。OSL 共通レジスタ v2 に対応する
 * (仕様は「OSL共通レジスタv2_アドレス表.md」)。
 *
 * 親機は子機ごとに「読み方を決めるのに要る最小限」だけを RAM に持つ。計測値も持たない。
 * 毎周期 POLL を読んだらその場で JSON にして publish し、読めなかったときは
 * 「通信失敗」の状態だけを送る (値はサーバが前回値のまま保持する)。
 * 名前・チャネル記述・識別は、config を送るときや Web 画面で子機から読み直す。
 */

#ifndef SLAVE_NODE_H
#define	SLAVE_NODE_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// 親機が同時に管理できる子機の上限
#define MAX_NODES               64

// 1 子機あたりの計測値 (チャネル) の最大数 (仕様 §3.2)
#define NODE_MAX_VALUES         16

// I²C アドレスの走査範囲 (7bit、予約域を除いた一般デバイス領域)
#define I2C_ADDR_SCAN_FIRST     0x08
#define I2C_ADDR_SCAN_LAST      0x77

// ===== 共通レジスタ v2 の番地 (16bit、上位バイトから送る) ===================
#define REG_IDENT               0x0000  // 識別 32 B (CRC 込み)
#define REG_ADDR_KEY            0x0020  // アドレス変更の鍵 + 新アドレス
#define REG_NAME                0x0040  // 名前 64 B + CRC 2 B
#define REG_CHDESC              0x0100  // チャネル記述 16 B × N + CRC
#define REG_POLL                0x0300  // POLL (sample_seq, value[N], status[N], CRC)
#define REG_EXT_IDENT           0x0400  // 拡張識別 8 B + CRC
#define REG_EXT_ATTR            0x0420  // 拡張チャネル属性 4 B × N + CRC
#define REG_EXT_STATUS          0x0480  // 拡張ステータス 10 B + CRC

#define IDENT_LEN               32
#define NAME_LEN                64      // UTF-8、NUL 終端 (本文は 63 B まで)
#define EXT_IDENT_LEN           10
#define EXT_STATUS_LEN          12
#define ADDR_KEY_UNLOCK         0xA5

// capability (拡張識別)
#define CAP_CH_ATTR             0x0001
#define CAP_EXT_STATUS          0x0002

// value_type (チャネル記述)
#define VTYPE_FLOAT32           0
#define VTYPE_INT32             1
#define VTYPE_UINT32            2

// チャネルの状態 (BACnet Reliability)。親機が子機を読めなかったときに全チャネルへ付ける。
#define ST_COMMUNICATION_FAILURE 12
#define ST_UNRELIABLE_OTHER      7

// 親機側 RAM に保持する子機の状態
typedef struct {
    uint8_t  i2c_addr;                  // 7bit
    uint8_t  data_count;                // チャネル数 (≤ NODE_MAX_VALUES)
    bool     has_ext_status;            // 拡張ステータスを実装している
    bool     comm_ok;                   // 直近の POLL を読めた
    uint32_t type_bits;                 // チャネルごとの value_type (2bit × 16)
    uint64_t prec_bits;                 // チャネルごとの有効桁数 (4bit × 16、0 = 既定の 6 桁)

    // 診断の変化検出用 (拡張ステータス)
    uint16_t err_i2c;
    uint16_t err_sensor;
    uint32_t uptime_s;
} slave_node_t;


#ifdef	__cplusplus
}
#endif

#endif	/* SLAVE_NODE_H */
