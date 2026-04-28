#include <stdio.h>
#include <string.h>
#include <math.h>
#include "node_manager.h"
#include "i2c_master.h"

slave_node_t NM_Nodes[MAX_NODES];
uint8_t      NM_NodeCount = 0;

void NM_init(void)
{
    NM_NodeCount = 0;
    memset(NM_Nodes, 0, sizeof(NM_Nodes));
}

// 指定した I²C アドレスの子機から共通領域の固定情報 (0x00~0x17) を読み取り、
// node に格納する。検出と同時に呼ばれる前提で、status/value はゼロ初期化のまま、
// seq は SEQ_INITIAL に設定する。
static bool readInfoBlock(uint8_t i2c_addr, slave_node_t* node)
{
    uint8_t reg = INFO_BLOCK_OFFSET;
    uint8_t buf[INFO_BLOCK_LEN];

    if (!I2C_WriteRead(i2c_addr, &reg, 1, buf, INFO_BLOCK_LEN)) return false;

    node->i2c_addr = i2c_addr;

    // 0x00~0x03: Device ID (uint32 LE)
    node->device_id =  ((uint32_t)buf[0])
                    | (((uint32_t)buf[1]) << 8)
                    | (((uint32_t)buf[2]) << 16)
                    | (((uint32_t)buf[3]) << 24);

    // 0x06: Data Count
    uint8_t cnt = buf[REG_DATA_COUNT - INFO_BLOCK_OFFSET];
    if (cnt > NODE_MAX_VALUES) cnt = NODE_MAX_VALUES;
    node->data_count = cnt;

    // 0x08~0x17: Unit Type[8] (uint16 LE × 8)
    for (uint8_t i = 0; i < NODE_MAX_VALUES; i++) {
        uint8_t off = (REG_UNIT_TYPE - INFO_BLOCK_OFFSET) + i * 2;
        node->unit_type[i] = ((uint16_t)buf[off]) | (((uint16_t)buf[off + 1]) << 8);
    }

    // 計測関連は未取得状態で初期化
    node->status1 = 0;
    node->status2 = 0;
    node->seq     = SEQ_INITIAL;
    memset(node->value, 0, sizeof(node->value));

    return true;
}

uint8_t NM_scan(void)
{
    NM_NodeCount = 0;
    memset(NM_Nodes, 0, sizeof(NM_Nodes));

    for (uint8_t addr = I2C_ADDR_SCAN_FIRST;
         addr <= I2C_ADDR_SCAN_LAST && NM_NodeCount < MAX_NODES;
         addr++)
    {
        if (!I2C_IsConnected(addr)) continue;

        if (readInfoBlock(addr, &NM_Nodes[NM_NodeCount])) {
            NM_NodeCount++;
        }
        // 固有情報の読み取りに失敗したアドレスは黙って捨てる (次回 SCAN で復活する可能性)
    }

    return NM_NodeCount;
}

bool NM_pollOne(slave_node_t* node)
{
    uint8_t reg = POLL_BLOCK_OFFSET;
    uint8_t buf[POLL_BLOCK_LEN];

    if (!I2C_WriteRead(node->i2c_addr, &reg, 1, buf, POLL_BLOCK_LEN)) return false;

    // 0x18: Status1
    node->status1 = buf[REG_STATUS1 - POLL_BLOCK_OFFSET];
    // 0x19: Status2
    node->status2 = buf[REG_STATUS2 - POLL_BLOCK_OFFSET];
    // 0x1C~0x3B: float[8] LE
    // AVR は LE なので memcpy で OK
    memcpy(node->value,
           &buf[REG_VALUE - POLL_BLOCK_OFFSET],
           sizeof(float) * NODE_MAX_VALUES);

    return true;
}

bool NM_clearStatus2(slave_node_t* node)
{
    uint8_t writeBuf[2] = { REG_STATUS2, 0 };
    return I2C_Write(node->i2c_addr, writeBuf, sizeof(writeBuf));
}

// MQTT トピック生成 ----------------------------------------------------------

void NM_buildConfigTopic(const slave_node_t* node, char* buf, uint16_t bufsize)
{
    snprintf(buf, bufsize, "nodes/config/%08lX", (unsigned long)node->device_id);
}

void NM_buildDataTopic(const slave_node_t* node, char* buf, uint16_t bufsize)
{
    snprintf(buf, bufsize, "nodes/data/%08lX", (unsigned long)node->device_id);
}

// JSON ビルダ -----------------------------------------------------------------

// 浮動小数点を有効数字 4 桁に丸める。
// NaN/Inf はそのまま返す (snprintf 側で "nan"/"inf" になる)。
static float roundTo4Sigfig(float x)
{
    if (!isfinite(x) || x == 0.0f) return x;
    float magnitude = floorf(log10f(fabsf(x)));
    float factor    = powf(10.0f, 3 - magnitude); // 4 桁目で丸める
    return roundf(x * factor) / factor;
}

uint16_t NM_buildConfigJson(const slave_node_t* node, char* buf, uint16_t bufsize)
{
    if (bufsize == 0) return 0;
    char* p   = buf;
    char* end = buf + bufsize;
    int   n;

    n = snprintf(p, end - p, "{\"id\":\"%08lX\",\"cnt\":%u,\"u\":[",
                 (unsigned long)node->device_id, (unsigned)node->data_count);
    if (n < 0 || n >= end - p) return 0;
    p += n;

    for (uint8_t i = 0; i < node->data_count; i++) {
        n = snprintf(p, end - p, "%s%u",
                     (i == 0 ? "" : ","),
                     (unsigned)node->unit_type[i]);
        if (n < 0 || n >= end - p) return 0;
        p += n;
    }

    n = snprintf(p, end - p, "]}");
    if (n < 0 || n >= end - p) return 0;
    p += n;

    return (uint16_t)(p - buf);
}

uint16_t NM_buildDataJson(const slave_node_t* node, char* buf, uint16_t bufsize)
{
    if (bufsize == 0) return 0;
    char* p   = buf;
    char* end = buf + bufsize;
    int   n;

    n = snprintf(p, end - p, "{\"id\":\"%08lX\",\"st\":%u,\"sq\":%u,\"v\":[",
                 (unsigned long)node->device_id,
                 (unsigned)node->status1,
                 (unsigned)node->seq);
    if (n < 0 || n >= end - p) return 0;
    p += n;

    for (uint8_t i = 0; i < node->data_count; i++) {
        float v = roundTo4Sigfig(node->value[i]);
        // %g は有効数字 6 桁が既定。precision を 4 に絞り、不要なゼロは省略させる。
        n = snprintf(p, end - p, "%s%.4g",
                     (i == 0 ? "" : ","),
                     (double)v);
        if (n < 0 || n >= end - p) return 0;
        p += n;
    }

    n = snprintf(p, end - p, "]}");
    if (n < 0 || n >= end - p) return 0;
    p += n;

    return (uint16_t)(p - buf);
}
