/*
 * File:   node_manager.h
 * Author: e.togashi
 *
 * 子機リストの管理、I²C 走査、共通レジスタ v2 へのアクセス、MQTT v2 の JSON の組み立て。
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

// I²C アドレス 0x08~0x77 を走査し、OSL v2 子機のリストを作り直す (仕様 §7.1)。
// 番地を送らない読み出しで "OSL" と CRC を確かめ、正体の分からない機器には書き込まない。
// OSL 子機でなかったアドレスは覚えておき、NM_scanNewNodes() では読みに行かない。
// 戻り値: 検出した子機の数。
uint8_t NM_scan(void);

// 増分スキャン: 既存リストにも「OSL 子機でない」記録にも無いアドレスだけを調べ、
// 応答した OSL 子機を末尾に追加する。既存ノードの状態は保持する。
// 戻り値: 新規に追加した子機の数。
uint8_t NM_scanNewNodes(void);

// 識別区画を読む (CRC 確認済み)。buf は IDENT_LEN バイト。
bool NM_readIdent(const slave_node_t* node, uint8_t* buf);

// 名前を子機から読む。buf は NAME_LEN バイト以上。失敗なら false (buf は空文字)。
bool NM_readName(const slave_node_t* node, char* buf);

// 名前を書く (UTF-8、63 バイトを超える分は文字の途中で切れないように切り詰める)。
// 子機は STOP を受けたら EEPROM に保存する。戻り値: I²C 書き込みの成否。
bool NM_writeName(const slave_node_t* node, const char* new_name);

// I²C アドレスを変える。鍵 0xA5 と新アドレスを 1 回の書き込みで送る。
// 子機は STOP の直後から新アドレスで応答する。成功時は node->i2c_addr も更新する。
// new_addr は 0x08-0x77。範囲外、自アドレスと同じ、I²C 失敗のいずれかで false。
bool NM_writeAddress(slave_node_t* node, uint8_t new_addr);

// 拡張ステータスを読む。未実装または失敗なら false。
bool NM_readExtStatus(const slave_node_t* node, uint32_t* uptime_s, uint8_t* reset_cause,
                      uint16_t* err_i2c, uint16_t* err_sensor);

// ===== MQTT v2 (トピックは "poem/<親機ID>/node/<adr>/...") =====================

// "poem/<pod>/node/<adr>/<kind>" を作る。kind = "config" / "data" / "diag"。
void NM_buildTopic(const slave_node_t* node, const char* pod_id, const char* kind,
                   char* buf, uint16_t bufsize);

// config: {"uid","vendor","model","fw","name","period","ch":[{"n","u","t","p","f"},…]}
// 識別・名前・チャネル記述・拡張チャネル属性を子機から読んで組み立てる。
// 戻り値: 書き込まれたバイト数 (NUL 終端は含まない)。失敗時は 0。
uint16_t NM_buildConfigJson(const slave_node_t* node, char* buf, uint16_t bufsize);

// data: POLL を読み、その場で {"sq","st":[…],"v":[…]} を組み立てる。
// 読めなかったとき (I2C 失敗・CRC 不一致) は {"st":[12,…],"v":[]} を組み立て、false を返す。
// 値は型どおりに出す (整数型は整数のまま、float は有効桁数で丸める)。
// *len に書き込んだバイト数を返す (組み立てられなければ 0)。
bool NM_pollAndBuildDataJson(slave_node_t* node, char* buf, uint16_t bufsize, uint16_t* len);

// UTF-8 文字列を max_bytes 以内に切り詰める (多バイト文字の途中では切らない)
void NM_truncateUtf8(char* s, uint16_t max_bytes);


#ifdef	__cplusplus
}
#endif

#endif	/* NODE_MANAGER_H */
