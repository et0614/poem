/*
 * File:   sysex.h
 * Author: e.togashi
 *
 * SysEx 層 (仕様の L2 の下半分)。USB-MIDI のパケットから SysEx を組み立て、
 * 応答を USB-MIDI のパケットに分けて送る。7bit 符号化 (8-to-7) もここで行う。
 * メッセージの中身 (コマンド) は bridge.c が解釈する。
 */

#ifndef SYSEX_H
#define	SYSEX_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// 受信できる SysEx の最大長 (F0 / F7 を除く、符号化されたまま) [byte]。
// WRITE の最大 (アドレス 1 + データ 256 = 257 B) を符号化すると 294 B、
// ヘッダを足して約 300 B。BATCH の要求もこの範囲に収める。
#define SYSEX_RX_MAX    400

// 受信したパケットを処理する。メインループから毎回呼ぶ。
// SysEx が 1 つ揃うたびに BRIDGE_OnSysEx() を呼ぶ。
void SYSEX_Task(void);

// 7bit 符号化されたバイト列をその場で復号する。戻り値は復号後の長さ。
// 形が不正なら *ok = false。
uint16_t SYSEX_Decode7(uint8_t *buf, uint16_t len, bool *ok);

// 応答を送る: F0 <MFR> <PRODUCT> <seq> <cmd | 0x40> <payload を符号化> F7
// 送信バッファが空くまで tud_task() を回して待つ。USB が切れたら false。
bool SYSEX_SendResponse(uint8_t seq, uint8_t cmd, const uint8_t *payload, uint16_t len);

// 符号化せずにそのまま送る (Device Inquiry の応答用)。msg は F0〜F7 を含む。
bool SYSEX_SendRaw(const uint8_t *msg, uint16_t len);

#ifdef	__cplusplus
}
#endif

#endif	/* SYSEX_H */
