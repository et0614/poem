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

// スキャン (NM_scan / NM_scanNewNodes) を実行した累計。診断用。
extern uint16_t     NM_ScanCount;

// 起動時に呼び出す。リストを空にする。
void NM_init(void);

// I²C アドレス 0x08~0x77 を走査し、応答した子機の固有情報 (0x00~0x17)
// を読み取って NM_Nodes[] を再構築する。直前の検出状態は破棄される。
// 戻り値: 検出した子機の数。
uint8_t NM_scan(void);

// 増分スキャン: 既存リストに無いアドレスだけを走査し、応答した子機を末尾に追加する。
// 既存ノードの状態は保持される。稼働中の増設検出 (定期実行) 用。
// 戻り値: 新規に追加した子機の数。
uint8_t NM_scanNewNodes(void);

// 指定した子機の状態 (Status1/2 + Value) を I²C で読み取り、構造体に反映する。
// 戻り値: true = 読み取り成功、false = 通信失敗。
bool NM_pollOne(slave_node_t* node);

// 子機の Status2 に 0 を書き戻す (新値受領のハンドシェイク)。
// 戻り値: true = 成功、false = I²C 失敗。
bool NM_clearStatus2(slave_node_t* node);

// 子機の name レジスタ (0x18-0x27) を更新する。子機側 EEPROM へ自動的に永続化される。
// 同時にローカル cache (node->name) も更新する。new_name は NUL 終端文字列で、
// NODE_NAME_LEN-1 文字以降は切り捨てられる。
// 戻り値: true = I²C 書き込み成功、false = 失敗。
bool NM_writeName(slave_node_t* node, const char* new_name);

// 子機の I²C アドレスを変更する。REG_ADDR_KEY に解錠鍵を、続けて REG_NEW_ADDR に
// 新アドレスを書き込む 1 トランザクション。子機は STOP 受信で EEPROM 永続化と
// TWI スレーブアドレス (SADDR) 反映を同時に行う仕様なので、リセット不要で即時切替。
// 成功時はローカル cache (node->i2c_addr) も新値に更新する。
// new_addr は 0x08-0x77 の範囲。範囲外、自アドレスと同じ、I²C 失敗のいずれかで false。
bool NM_writeAddress(slave_node_t* node, uint8_t new_addr);

// MQTT トピック生成ヘルパ。"nodes/config/{8桁hex}" / "nodes/data/{8桁hex}"。
// buf には 24 byte 程度を確保すれば十分 (例: "nodes/config/12345678" は 21 文字 + NUL)。
void NM_buildConfigTopic(const slave_node_t* node, char* buf, uint16_t bufsize);
void NM_buildDataTopic  (const slave_node_t* node, char* buf, uint16_t bufsize);

// config トピック向け JSON を構築する。Schema 図 3 準拠 ({id, cnt, u})。
// 戻り値: 書き込まれたバイト数 (NUL 終端は含まない)。失敗時は 0。
uint16_t NM_buildConfigJson(const slave_node_t* node, char* buf, uint16_t bufsize);

// data トピック向け JSON を構築する。Schema 図 4 準拠 ({id, st, sq, v})。
// st は status1 の値そのまま、sq は親機管理シーケンス番号、v は有効数字 6 桁丸め。
// 戻り値: 書き込まれたバイト数。失敗時は 0。
uint16_t NM_buildDataJson(const slave_node_t* node, char* buf, uint16_t bufsize);


#ifdef	__cplusplus
}
#endif

#endif	/* NODE_MANAGER_H */
