using System.Text.Json.Serialization;

namespace PoEMServer
{
    /// <summary>
    /// MQTT topic "nodes/config/&lt;hex&gt;" のペイロード。
    /// 親機 (poem_pod) が SCAN 時に各子機について 1 回 publish する固定情報。
    /// firmware/poem_pod.X/node_manager.c::NM_buildConfigJson と同期。
    /// </summary>
    internal class NodeConfigPayload
    {
        /// <summary>子機の DeviceID (FNV-1a 32bit ハッシュ、8 桁 HEX)</summary>
        [JsonPropertyName("id")]
        public string IdHex { get; set; } = "";

        /// <summary>装置ラベル (子機の EEPROM 永続、最大 15 文字)</summary>
        [JsonPropertyName("name")]
        public string Name { get; set; } = "";

        /// <summary>有効計測値の個数 (≤ 8)</summary>
        [JsonPropertyName("cnt")]
        public int DataCount { get; set; }

        /// <summary>各計測値の BACnet 単位コード (ASHRAE 135 EngineeringUnits)</summary>
        [JsonPropertyName("u")]
        public ushort[] UnitTypes { get; set; } = System.Array.Empty<ushort>();
    }

    /// <summary>
    /// MQTT topic "nodes/data/&lt;hex&gt;" のペイロード。
    /// 親機が毎周期 (1 秒) publish する計測データ。
    /// firmware/poem_pod.X/node_manager.c::NM_buildDataJson と同期。
    /// </summary>
    internal class NodeDataPayload
    {
        /// <summary>子機の DeviceID (8 桁 HEX)</summary>
        [JsonPropertyName("id")]
        public string IdHex { get; set; } = "";

        /// <summary>Status1: per-value bitmask (bit i = value[i] が stale)、0xFF = 全停止</summary>
        [JsonPropertyName("st")]
        public byte Status1 { get; set; }

        /// <summary>親機側で管理されるシーケンス番号 (0..254、255=未計測)</summary>
        [JsonPropertyName("sq")]
        public byte Seq { get; set; }

        /// <summary>計測値配列 (有効長は DataCount、超過分は無視)</summary>
        [JsonPropertyName("v")]
        public float[] Values { get; set; } = System.Array.Empty<float>();
    }
}
