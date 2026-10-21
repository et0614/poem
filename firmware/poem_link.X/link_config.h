/*
 * File:   link_config.h
 * Author: e.togashi
 *
 * PoEM Link 全体で共有する定数。値の根拠は「PoEM_Link_通信仕様.md」を参照。
 */

#ifndef LINK_CONFIG_H
#define	LINK_CONFIG_H

#ifdef	__cplusplus
extern "C" {
#endif

// ファームウェアの版。GET_INFO と Device Inquiry で返す。
#define FW_VERSION_MAJOR        0
#define FW_VERSION_MINOR        1
#define FW_VERSION_PATCH        0

// SysEx のプロトコル版 (仕様 §4.2)
#define PROTOCOL_VERSION        1

// SysEx のメーカー ID (仕様 §3.1)。
// 開発中は非商用 ID の 0x7D (1 バイト)。正式な ID を取得したら 3 バイト
// (00 xx xx) に変え、SYSEX_MFR_LEN を 3 にする。
#define SYSEX_MFR_LEN           1
#define SYSEX_MFR_0             0x7D
#define SYSEX_MFR_1             0x00
#define SYSEX_MFR_2             0x00

// 製品コード。01 = E-Sensor (予約)、02 = PoEM Link
#define SYSEX_PRODUCT           0x02

// Device Inquiry の family / model (仕様 §3.4)
#define DI_FAMILY               0x0002
#define DI_MODEL                0x0001

// 上限値 (仕様 §6.1)
#define MAX_XFER                256     // 1 回の I2C 転送の上限 [byte]
#define MAX_BATCH               512     // BATCH の応答データの上限 [byte]
#define MAX_BATCH_OPS           64      // BATCH の操作数の上限

// I2C の既定値 (仕様 §6.1)。SET_CONFIG で変えられるが、再起動で戻る。
#define I2C_DEFAULT_KHZ         100
#define I2C_DEFAULT_TIMEOUT_MS  100
#define I2C_TIMEOUT_MIN_MS      10
#define I2C_TIMEOUT_MAX_MS      1000

// 転送の開始前にバスの空きを待つ時間 [ms]。動作中の親機 (Pod) のバスに誤って
// つながれたとき、通信を割り込ませないため。
#define I2C_IDLE_WAIT_MS        5

#ifdef	__cplusplus
}
#endif

#endif	/* LINK_CONFIG_H */
