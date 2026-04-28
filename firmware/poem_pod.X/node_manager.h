/*
 * File:   node_manager.h
 * Author: e.togashi
 *
 * 子機リストの管理、I²C 走査、共通レジスタアクセスを担う。
 */

#ifndef NODE_MANAGER_H
#define	NODE_MANAGER_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "slave_node.h"

// 親機が把握している子機リスト
extern slave_node_t NM_Nodes[MAX_NODES];
extern uint8_t      NM_NodeCount;

// 起動時に呼び出す。リストを空にする。
void NM_init(void);

// I²C アドレス 0x08~0x77 を走査し、応答した子機の固有情報 (0x00~0x17)
// を読み取って NM_Nodes[] を再構築する。直前の検出状態は破棄される。
// 戻り値: 検出した子機の数。
uint8_t NM_scan(void);

// 指定した子機の状態 (Status1/2 + Value) を I²C で読み取り、構造体に反映する。
// 戻り値: true = 読み取り成功、false = 通信失敗。
bool NM_pollOne(slave_node_t* node);

// 子機の Status2 に 0 を書き戻す (新値受領のハンドシェイク)。
// 戻り値: true = 成功、false = I²C 失敗。
bool NM_clearStatus2(slave_node_t* node);

// MQTT トピック生成ヘルパ。"nodes/config/{8桁hex}" / "nodes/data/{8桁hex}"。
// buf には 24 byte 程度を確保すれば十分 (例: "nodes/config/12345678" は 21 文字 + NUL)。
void NM_buildConfigTopic(const slave_node_t* node, char* buf, uint16_t bufsize);
void NM_buildDataTopic  (const slave_node_t* node, char* buf, uint16_t bufsize);

// config トピック向け JSON を構築する。Schema 図 3 準拠 ({id, cnt, u})。
// 戻り値: 書き込まれたバイト数 (NUL 終端は含まない)。失敗時は 0。
uint16_t NM_buildConfigJson(const slave_node_t* node, char* buf, uint16_t bufsize);

// data トピック向け JSON を構築する。Schema 図 4 準拠 ({id, st, sq, v})。
// st は status1 の値そのまま、sq は親機管理シーケンス番号、v は有効数字 4 桁丸め。
// 戻り値: 書き込まれたバイト数。失敗時は 0。
uint16_t NM_buildDataJson(const slave_node_t* node, char* buf, uint16_t bufsize);


#ifdef	__cplusplus
}
#endif

#endif	/* NODE_MANAGER_H */
