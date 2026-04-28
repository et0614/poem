/*
 * File:   mqtt_app.h
 * Author: e.togashi
 *
 * MQTT クライアントの接続/維持/送信ヘルパ。
 * W5500 ドライバとの接続管理を一箇所に閉じ込める。
 */

#ifndef MQTT_APP_H
#define	MQTT_APP_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "W5500/MQTTClient.h"

// MQTT 通信に使う TCP ソケット番号
#define MQTT_TCP_SOCKET     0

// MQTT 用ポート (ブローカー側)
#define MQTT_PORT           1883

// MQTT 送受信バッファサイズ
#define MQTT_BUF_SIZE       2048

// 親機グローバルの MQTT クライアント (内部状態保持)
extern MQTTClient MA_Client;

// ブローカーへ接続を試みる。MAC アドレス・IP・ブローカーIPは EM_Network_Settings を参照。
// 戻り値: true = 接続成功 (MA_Client.isconnected が true)、false = 失敗。
bool MA_connect(void);

// 切断する (LWT 等は使用しない)。
void MA_disconnect(void);

// 接続状態を確認する。socket の状態と MQTT 層の isconnected の両方を見る。
bool MA_isConnected(void);

// MQTT 接続を維持する (keepalive PING など)。メインループで定期的に呼ぶ。
void MA_yield(uint32_t timeout_ms);

// 指定トピックへ payload を QoS0/non-retained で送信する。
// 戻り値: true = MQTTPublish 成功、false = 失敗 (上位で再接続トリガ)。
bool MA_publish(const char* topic, const uint8_t* payload, uint16_t payload_len);


#ifdef	__cplusplus
}
#endif

#endif	/* MQTT_APP_H */
