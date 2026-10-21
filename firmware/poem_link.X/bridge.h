/*
 * File:   bridge.h
 * Author: e.togashi
 *
 * コマンド層 (仕様の L2 の上半分)。SysEx のヘッダを確かめ、コマンドを振り分け、
 * 汎用 I2C 層 (i2c_host) を呼んで応答を組み立てる。
 */

#ifndef BRIDGE_H
#define	BRIDGE_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// 揃った SysEx を 1 つ処理する (F0 / F7 を除いた本体)。sysex.c から呼ばれる。
// msg はその場で復号に使うので書き換わる。truncated は長すぎて末尾を捨てたとき true。
void BRIDGE_OnSysEx(uint8_t *msg, uint16_t len, bool truncated);

#ifdef	__cplusplus
}
#endif

#endif	/* BRIDGE_H */
