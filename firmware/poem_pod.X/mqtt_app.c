#include <stdio.h>
#include <string.h>
#include <avr/io.h>     // SIGROW
#include <avr/wdt.h>    // wdt_reset (長時間ブロックする MQTT/TCP 操作中に WDT を生かす)

#include "mqtt_app.h"
#include "eeprom_manager.h"
#include "W5500/w5500.h"
#include "W5500/socket.h"
#include "W5500/wizchip_conf.h"

// MQTT 層のコマンド応答待ち [ms]。MQTTConnect の CONNACK 待ちにも使われる。
// WDT (4.1s) を超えないよう短めに設定する。TCP connect (~1.6s) と合わせても余裕がある値。
#define MQTT_COMMAND_TIMEOUT_MS  1500

// MQTT クライアント本体 (W5500 ライブラリの構造体)
MQTTClient MA_Client;

// MQTT 接続用ネットワーク構造体は静的に確保。
// classic 実装ではローカル変数だったが、MQTTClientInit がポインタを保持するため
// スコープを永続化する必要がある。
static Network  s_mqtt_net;

// 送受信バッファ。HTTP 設定モードと MQTT 通信モードは同時に動かないので、両方で
// 共用する (別々に持つと 4 KB 余分にかかり、SRAM 16 KB のうち 15 KB に達していた)。
uint8_t g_io_tx_buf[MQTT_BUF_SIZE];
uint8_t g_io_rx_buf[MQTT_BUF_SIZE];

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

void MA_initW5500(void)
{
    // W5500 内蔵 RAM を 8 ソケットへ均等割当 (各 TX/RX = 2KB)
    uint8_t txsize[8] = {2,2,2,2,2,2,2,2};
    uint8_t rxsize[8] = {2,2,2,2,2,2,2,2};
    wizchip_init(txsize, rxsize);

    // TCP 再送タイムアウトをチップ既定 (RTR=2000=200ms, RCR=8 → 約 1.8s) より短くする。
    // ブローカ未到達時の connect() ブロック時間を最小化することで、HTTP の応答性を維持する。
    // RTR=500 (50ms), RCR=2 → 約 150ms で TIMEOUT 割り込みが立つ。
    setRTR(500);
    setRCR(2);

    // MAC/IP/SN/GW/DNS を W5500 レジスタへ反映
    applyNetworkInfo();
}

const char* MA_PodId(void)
{
    static char id[13] = "";
    if (id[0] == '\0') {
        uint8_t mac[6];
        buildMacAddress(mac);
        snprintf(id, sizeof(id), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return id;
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
    // ブローカ未到達時に TCP connect が ~1.6s ブロックしうるため、各段で WDT を生かす。
    wdt_reset();

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

    wdt_reset();   // TCP 接続成功後、MQTT CONNACK 待ち (~1.5s) の前に WDT リフレッシュ

    // MQTT クライアント初期化 (静的バッファを渡す)
    MQTTClientInit(&MA_Client, &s_mqtt_net, MQTT_COMMAND_TIMEOUT_MS,
                   g_io_tx_buf, sizeof(g_io_tx_buf),
                   g_io_rx_buf, sizeof(g_io_rx_buf));

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
