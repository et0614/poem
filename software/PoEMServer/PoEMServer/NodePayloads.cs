using System.Text.Json.Serialization;

namespace PoEMServer
{
    // MQTT v2 のペイロード。親機 (firmware/poem_pod.X) の node_manager.c / main.c と同期する。
    //
    // トピック:
    //   poem/<pod>/pod/info              親機の情報 (接続のたび)
    //   poem/<pod>/pod/diag              親機の I2C 診断 (毎秒、ログに出すだけ)
    //   poem/<pod>/node/<adr>/config     子機の固定情報 (走査時・増設時)
    //   poem/<pod>/node/<adr>/data       子機の計測値 (毎秒)
    //   poem/<pod>/node/<adr>/diag       子機の拡張ステータス (変化時)
    // <pod> は親機の MAC アドレス 12 桁、<adr> は子機の I2C アドレス (10 進)。

    /// <summary>poem/&lt;pod&gt;/pod/info</summary>
    internal class PodInfoPayload
    {
        /// <summary>親機のファームウェアの版 ("2.0.0")</summary>
        [JsonPropertyName("fw")]
        public string Firmware { get; set; } = "";

        /// <summary>親機名 (Web 画面で設定する。空のこともある)</summary>
        [JsonPropertyName("name")]
        public string Name { get; set; } = "";

        /// <summary>親機の IP アドレス</summary>
        [JsonPropertyName("ip")]
        public string Ip { get; set; } = "";
    }

    /// <summary>子機のチャネル記述 (OSL 共通レジスタ v2 のチャネル記述 + 拡張チャネル属性)</summary>
    internal class ChannelDesc
    {
        /// <summary>チャネル名 ("Temp" など)</summary>
        [JsonPropertyName("n")]
        public string Name { get; set; } = "";

        /// <summary>BACnet Engineering Units</summary>
        [JsonPropertyName("u")]
        public ushort Unit { get; set; }

        /// <summary>値の型 (0 = float32, 1 = int32, 2 = uint32)</summary>
        [JsonPropertyName("t")]
        public byte Type { get; set; }

        /// <summary>有効桁数 (0 = 既定)</summary>
        [JsonPropertyName("p")]
        public byte Precision { get; set; }

        /// <summary>bit0 = 積算値、bit1 = 補助値</summary>
        [JsonPropertyName("f")]
        public byte Flags { get; set; }
    }

    /// <summary>poem/&lt;pod&gt;/node/&lt;adr&gt;/config</summary>
    internal class NodeConfigPayload
    {
        /// <summary>個体 ID (16 B の device_id を 16 進 32 文字)</summary>
        [JsonPropertyName("uid")]
        public string Uid { get; set; } = "";

        [JsonPropertyName("vendor")]
        public ushort Vendor { get; set; }

        [JsonPropertyName("model")]
        public ushort Model { get; set; }

        /// <summary>子機のファームウェアの版</summary>
        [JsonPropertyName("fw")]
        public string Firmware { get; set; } = "";

        /// <summary>新しい値を公開する周期 [ms]</summary>
        [JsonPropertyName("period")]
        public ushort PeriodMs { get; set; }

        /// <summary>利用者が付けた呼び名 (UTF-8、63 バイトまで)</summary>
        [JsonPropertyName("name")]
        public string Name { get; set; } = "";

        [JsonPropertyName("ch")]
        public ChannelDesc[] Channels { get; set; } = System.Array.Empty<ChannelDesc>();
    }

    /// <summary>poem/&lt;pod&gt;/node/&lt;adr&gt;/data</summary>
    internal class NodeDataPayload
    {
        /// <summary>sample_seq (子機が新しい値を公開するたびに +1)。親機が子機を読めなかったときは無い</summary>
        [JsonPropertyName("sq")]
        public ushort? Seq { get; set; }

        /// <summary>チャネルごとの状態 (BACnet Reliability の値。0 = 正常、12 = 通信失敗、64 = 準備中)</summary>
        [JsonPropertyName("st")]
        public uint[] Status { get; set; } = System.Array.Empty<uint>();

        /// <summary>計測値。親機が子機を読めなかったときは空</summary>
        [JsonPropertyName("v")]
        public double[] Values { get; set; } = System.Array.Empty<double>();
    }

    /// <summary>poem/&lt;pod&gt;/node/&lt;adr&gt;/diag</summary>
    internal class NodeDiagPayload
    {
        /// <summary>稼働時間 [s]</summary>
        [JsonPropertyName("up")]
        public uint UptimeSec { get; set; }

        /// <summary>直近の起動要因 (1 = 電源投入、2 = 電圧低下、3 = ウォッチドッグ、4 = ソフトウェア、5 = 外部)</summary>
        [JsonPropertyName("rc")]
        public byte ResetCause { get; set; }

        /// <summary>I2C の異常回数 (一巡する)</summary>
        [JsonPropertyName("err")]
        public ushort ErrI2c { get; set; }

        /// <summary>センサとの通信の異常回数 (一巡する)</summary>
        [JsonPropertyName("serr")]
        public ushort ErrSensor { get; set; }
    }
}
