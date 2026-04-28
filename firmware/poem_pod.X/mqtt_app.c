#include <stdio.h>
#include <string.h>
#include <avr/io.h>     // SIGROW

#include "mqtt_app.h"
#include "eeprom_manager.h"
#include "W5500/w5500.h"
#include "W5500/socket.h"
#include "W5500/wizchip_conf.h"

// MQTT クライアント本体 (W5500 ライブラリの構造体)
MQTTClient MA_Client;

// MQTT 接続用ネットワーク構造体・送受信バッファは静的に確保。
// classic 実装ではローカル変数だったが、MQTTClientInit がポインタを保持するため
// スコープを永続化する必要がある。
static Network  s_mqtt_net;
static uint8_t  s_mqtt_tx_buf[MQTT_BUF_SIZE];
static uint8_t  s_mqtt_rx_buf[MQTT_BUF_SIZE];

// MAC アドレス: 上 3 バイト = WIZnet OUI (00:08:DC)、下 3 バイト = AVR の SIGROW
// シリアル番号 (SERNUM7..9) で代用する。チップ固有なので装置間で重複しにくい。
static void buildMacAddress(uint8_t mac[6])
{
    mac[0] = 0x00;
    mac[1] = 0x08;
    mac[2] = 0xDC;
    mac[3] = SIGROW.SERNUM7;
    mac[4] = SIGROW.SERNUM8;
    mac[5] = SIGROW.SERNUM9;
}

// W5500 にネットワーク情報 (MAC + IP + SN + GW + DNS) を設定する。
// EM_Network_Settings から読み出して反映する。
static void applyNetworkInfo(void)
{
    wiz_NetInfo netinfo;

    buildMacAddress(netinfo.mac);
    memcpy(netinfo.ip,  EM_Network_Settings.ip,  4);
    memcpy(netinfo.sn,  EM_Network_Settings.sn,  4);
    memcpy(netinfo.gw,  EM_Network_Settings.gw,  4);
    memcpy(netinfo.dns, EM_Network_Settings.dns, 4);
    netinfo.dhcp = NETINFO_STATIC;

    ctlnetwork(CN_SET_NETINFO, (void*)&netinfo);
}

// MQTT クライアント ID は装置固有である必要がある。MAC アドレスを 16 進文字列化。
static void buildClientId(char* buf, size_t bufsize)
{
    uint8_t mac[6];
    buildMacAddress(mac);
    snprintf(buf, bufsize, "%02X-%02X-%02X-%02X-%02X-%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool MA_connect(void)
{
    // W5500 にネットワーク情報を流し込む (再接続のたびに再設定して問題ない)
    applyNetworkInfo();

    // MQTT 用のネットワークインターフェースを初期化
    NewNetwork(&s_mqtt_net, MQTT_TCP_SOCKET);

    // ブローカーへ TCP 接続
    uint8_t broker_ip[4];
    memcpy(broker_ip, EM_Network_Settings.bip, 4);
    if (ConnectNetwork(&s_mqtt_net, broker_ip, MQTT_PORT) != SOCK_OK) {
        return false;
    }

    // MQTT クライアント初期化 (静的バッファを渡す)
    MQTTClientInit(&MA_Client, &s_mqtt_net, 3000,
                   s_mqtt_tx_buf, sizeof(s_mqtt_tx_buf),
                   s_mqtt_rx_buf, sizeof(s_mqtt_rx_buf));

    // CONNECT パケットの組み立て
    char client_id_buf[20];
    buildClientId(client_id_buf, sizeof(client_id_buf));

    MQTTPacket_connectData mqtt_data = MQTTPacket_connectData_initializer;
    mqtt_data.MQTTVersion       = 3;
    mqtt_data.clientID.cstring  = client_id_buf;
    mqtt_data.keepAliveInterval = 60;
    mqtt_data.cleansession      = 1;

    if (MQTTConnect(&MA_Client, &mqtt_data) != SUCCESSS) {
        return false;
    }

    return true;
}

void MA_disconnect(void)
{
    disconnect(MQTT_TCP_SOCKET);
    MA_Client.isconnected = 0;
}

bool MA_isConnected(void)
{
    return (MA_Client.isconnected != 0)
        && (getSn_SR(MQTT_TCP_SOCKET) == SOCK_ESTABLISHED);
}

void MA_yield(uint32_t timeout_ms)
{
    if (MA_Client.isconnected) {
        MQTTYield(&MA_Client, timeout_ms);
    }
}

bool MA_publish(const char* topic, const uint8_t* payload, uint16_t payload_len)
{
    if (!MA_isConnected()) return false;

    MQTTMessage msg;
    msg.qos        = QOS0;
    msg.retained   = 0;
    msg.dup        = 0;
    msg.id         = 0;
    msg.payload    = (void*)payload;
    msg.payloadlen = payload_len;

    return (MQTTPublish(&MA_Client, topic, &msg) == SUCCESSS);
}
