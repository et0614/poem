/* 
 * File:   i2c_master.h
 * Author: e.togashi
 *
 * Created on April 27, 2026, 12:44 PM
 */

#ifndef I2C_MASTER_H
#define	I2C_MASTER_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 接続確認 (Address NACK チェック)
bool I2C_IsConnected(uint8_t address);

// 書き込み (完了待ち機能付き)
bool I2C_Write(uint8_t address, const uint8_t *data, size_t len);

// 読み込み (完了待ち機能付き)
bool I2C_Read(uint8_t address, uint8_t *buffer, size_t len);

// 書き込み後に読み込み (Repeated Start 対応)
bool I2C_WriteRead(uint8_t address, const uint8_t *writeData, size_t writeLen, uint8_t *readBuffer, size_t readLen);

// 1バイト書いてStop (特殊用途)
bool I2C_WriteByteAndStop(uint8_t address, uint8_t data);

// I2C の診断情報。publish して外から観測する。
//
// 失敗を原因別に分けてあるのは、合計だけでは何が起きたのか判別できないため。
// 実測では「バスが数秒止まる外乱」と「転送を開始できずに即失敗し続ける状態」で
// 増え方がまったく違うが、1 本のカウンタでは同じ数字にしか見えなかった。
typedef struct {
    uint8_t  mstatus;       // 現在の TWI1.MSTATUS。下位 2bit が BUSSTATE (1=IDLE)
    uint8_t  failMstatus;   // 直近の失敗時の MSTATUS (復帰処理で消される前の値)
    uint16_t failCount;     // 失敗の合計 = failTimeout + failStart (NACK は含まない)
    uint16_t failTimeout;   // 内訳: 完了待ちタイムアウト。1 回あたり約 100 ミリ秒かかる
    uint16_t failStart;     // 内訳: 転送を開始できなかった。待たないので一気に増える
    uint16_t recoverCount;  // バス強制復帰を実行した累計
} i2c_diag_t;

// I2C の診断情報を取得する。
void I2C_GetDiag(i2c_diag_t *diag);

#ifdef	__cplusplus
}
#endif

#endif	/* I2C_MASTER_H */

