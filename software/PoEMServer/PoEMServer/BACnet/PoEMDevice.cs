using System.IO.BACnet;
using System.IO.BACnet.Storage;
using BaCSharp;

namespace PoEMServer.BACnet
{
    /// <summary>
    /// 親機 (poem_pod) 1 台ぶんの BACnet Device。MQTT v2 のメッセージを BACnet オブジェクトへ反映する。
    ///
    /// 親機ごとに Device を分ける (Device インスタンス番号と UDP ポートは PodRegistry が決める)。
    /// 子機は親機の走査時に config を、毎秒 data を publish する。本クラスは:
    ///   - 初めて受け取った I²C アドレスについては BACnet オブジェクトを動的に生成する
    ///   - 既知の子機については data を受け取るたびに PRESENT_VALUE を更新する
    ///
    /// BACnet インスタンス番号は子機の I²C アドレスから固定的に決まる (base = adr × 100):
    ///   - アドレス範囲 8..119 → base 800..11900。0..99 はデバイス自身用に予約
    ///   - 管理単位は「親機内のアドレス = 物理配置」。センサを故障交換して同じアドレスに
    ///     別個体を付けてもインスタンス番号は変わらず、UID がプロパティとして更新される
    ///   - 子機のアドレスを変更した場合は旧 base のオブジェクトを撤去し、新 base に再生成する
    ///
    /// オブジェクト割当 (base = adr × 100):
    ///   base+0   DateTimeValue    最終受信日時
    ///   base+1   CharStringValue  UID (16 B の device_id、16 進 32 文字)
    ///   base+2   CharStringValue  名前 (子機の EEPROM に保存されている呼び名)
    ///   base+3   AnalogInput      異常チャネルのビット (bit i = チャネル i の状態が正常でない)
    ///   base+4   AnalogInput      sample_seq
    ///   base+5   AnalogInput      チャネル数
    ///   base+6   DateTimeValue    最終正常データ受信日時 (全チャネルが正常のときだけ更新)
    ///   base+7   CharStringValue  機種 (名前と model_id)
    ///   base+8   CharStringValue  子機のファームウェアの版
    ///   base+9   AnalogInput      子機の稼働時間 [s] (拡張ステータスを持つ子機のみ更新)
    ///   base+10..base+25  AnalogInput  チャネル 0..15 (UNITS はチャネル記述の単位)
    ///
    /// チャネルの状態は子機が BACnet Reliability の値で送ってくる (0 = 正常、12 = 通信失敗、
    /// 64 = 準備中 など)。正常でないチャネルは PRESENT_VALUE を最終有効値のまま凍結し、
    /// STATUS_FLAGS の FAULT ビットと RELIABILITY でそれを示す。
    /// </summary>
    internal class PoEMDevice
    {
        #region 定数

        /// <summary>
        /// DATETIME を文字列で持つときの書式。ライブラリ (System.IO.BACnet.Storage.Property) が
        /// ParseExact("yyyy/MM/dd-HH:mm:ss.ff") で読むため、これに一致させる必要がある
        /// (bacnet-stack 互換。小数部はミリ秒ではなく 1/100 秒の 2 桁)。
        /// 小数部は <see cref="FormatDateTime"/> で付加する。
        /// </summary>
        private const string DATE_TIME_FORMAT = "yyyy/MM/dd-HH:mm:ss.";

        /// <summary>子機 1 つあたりの BACnet Instance 番号確保枠 (base = I²Cアドレス × この値)</summary>
        private const uint INSTANCE_BLOCK = 100;

        /// <summary>1 子機が公開できるチャネルの上限 (OSL 共通レジスタ v2 と同じ)</summary>
        private const int MAX_VALUES_PER_NODE = 16;

        /// <summary>I²C アドレスの有効範囲 (firmware の走査範囲と同じ)</summary>
        private const byte I2C_ADDR_MIN = 0x08;
        private const byte I2C_ADDR_MAX = 0x77;

        private const ushort BACNET_UNITS_NO_UNITS = 95;
        private const ushort BACNET_UNITS_SECONDS  = 73;
        private const uint   BACNET_RELIABILITY_NO_FAULT_DETECTED = 0;

        /// <summary>機種の表示名 (仕様 §6.1)</summary>
        private static readonly Dictionary<ushort, string> MODEL_NAMES = new()
        {
            [0x0101] = "温湿度センサ (SHT4x)",
            [0x0102] = "多点温度センサ (TMP117x3)",
            [0x0201] = "風速センサ (熱線式)",
            [0x0301] = "絶対圧センサ (BMP585)",
            [0x0302] = "差圧センサ (SDP810-125Pa)",
            [0x0303] = "差圧センサ (SDP810-500Pa)",
        };

        #endregion

        #region インスタンス変数

        public BACnetCommunicator Communicator { get; private set; }

        /// <summary>親機 ID (MAC アドレス 12 桁)</summary>
        public string PodId { get; }

        /// <summary>I²C アドレス → ChildNode の登録表</summary>
        private readonly Dictionary<byte, ChildNode> childrenByAddr = new();

        /// <summary>storage / children を保護するロック</summary>
        private readonly object syncLock = new();

        #endregion

        #region コンストラクタ

        /// <param name="podId">親機 ID (MAC アドレス 12 桁)</param>
        /// <param name="templatePath">Device オブジェクトのひな形 (PoEMDeviceStorage.xml)</param>
        /// <param name="deviceInstance">この親機の BACnet Device インスタンス番号</param>
        /// <param name="exclusivePort">この親機の BACnet/IP の専用ポート</param>
        /// <param name="localEndPointAddress">BACnet/IP で使う自分の IP アドレス</param>
        public PoEMDevice(string podId, string templatePath, uint deviceInstance, int exclusivePort, string localEndPointAddress)
        {
            PodId = podId;
            DeviceStorage storage = DeviceStorage.Load(templatePath, deviceInstance);
            Communicator = new BACnetCommunicator(storage, exclusivePort, localEndPointAddress);
            SetDeviceName($"PoEM_{podId}");
        }

        #endregion

        #region 公開メソッド

        /// <summary>poem/&lt;pod&gt;/pod/info: 親機名を Device の名前に反映する</summary>
        public void OnPodInfo(PodInfoPayload info)
        {
            lock (syncLock)
            {
                string name = string.IsNullOrWhiteSpace(info.Name) ? PodId : info.Name.Trim();
                SetDeviceName($"PoEM_{name}");
            }
        }

        /// <summary>
        /// poem/&lt;pod&gt;/node/&lt;adr&gt;/config: 新規アドレスならオブジェクトを生成し、既知なら変更を反映する。
        /// </summary>
        public void OnConfig(byte adr, NodeConfigPayload config)
        {
            if (adr < I2C_ADDR_MIN || adr > I2C_ADDR_MAX) return;
            lock (syncLock)
            {
                var channels = SanitizeChannels(config.Channels);

                // 同じ UID が別アドレスで登録済み → 子機のアドレス変更。旧ブロックを撤去する。
                var moved = childrenByAddr.Values.FirstOrDefault(c => c.Uid == config.Uid && c.I2cAddr != adr);
                if (moved != null)
                {
                    Console.WriteLine($"[INFO] pod {PodId}: uid {config.Uid} moved from I2C address {moved.I2cAddr} to {adr}. Rebuilding BACnet objects.");
                    RemoveBacnetObjects(moved);
                    childrenByAddr.Remove(moved.I2cAddr);
                }

                if (!childrenByAddr.TryGetValue(adr, out ChildNode? child))
                {
                    child = new ChildNode { I2cAddr = adr, InstanceBase = adr * INSTANCE_BLOCK };
                    child.Apply(config, channels);
                    childrenByAddr[adr] = child;
                    CreateBacnetObjects(child);
                    return;
                }

                // 同じアドレスに別個体 (センサ交換) または構成の変化 → オブジェクトを作り直す。
                // インスタンス番号はアドレス基準なので変わらない。
                if (child.Uid != config.Uid || !child.SameLayout(channels))
                {
                    if (child.Uid != config.Uid)
                        Console.WriteLine($"[INFO] pod {PodId}: I2C address {adr}: device replaced ({child.Uid} -> {config.Uid}). Rebuilding BACnet objects.");
                    RemoveBacnetObjects(child);
                    child.Apply(config, channels);
                    CreateBacnetObjects(child);
                    return;
                }

                // 構成は同じ: 名前・機種・版だけを更新する
                string oldName = child.Name;
                child.Apply(config, channels);
                if (oldName != child.Name) WriteCharString(child.InstanceBase + 2u, child.Name);
                WriteCharString(child.InstanceBase + 7u, ModelLabel(child.Model));
                WriteCharString(child.InstanceBase + 8u, child.Firmware);
            }
        }

        /// <summary>
        /// poem/&lt;pod&gt;/node/&lt;adr&gt;/data: 計測値と状態を反映する。config 未受信のアドレスは無視する (config 待ち)。
        /// </summary>
        public void OnData(byte adr, NodeDataPayload data)
        {
            lock (syncLock)
            {
                if (!childrenByAddr.TryGetValue(adr, out ChildNode? child)) return;

                uint b = child.InstanceBase;
                int n = child.Channels.Length;
                DateTime now = DateTime.Now;

                WriteDateTime(b + 0u, now);

                // 状態 (無いチャネルは通信失敗とみなす)
                uint faults = 0;
                for (int i = 0; i < n; i++)
                {
                    uint st = i < data.Status.Length ? data.Status[i] : 12u;
                    if (st != 0) faults |= 1u << i;
                    if (st != child.LastStatus[i])
                    {
                        WriteValueFault(b + 10u + (uint)i, st);
                        child.LastStatus[i] = st;
                    }
                }
                WriteAnalogInput(b + 3u, faults);
                if (faults == 0) WriteDateTime(b + 6u, now);

                if (data.Seq.HasValue) WriteAnalogInput(b + 4u, data.Seq.Value);

                // 値 (親機が子機を読めなかったときは空。PRESENT_VALUE は前回値のまま)。
                // 状態が正常でないチャネルは値を更新せず、最終有効値のまま凍結する。
                int nv = Math.Min(n, data.Values.Length);
                for (int i = 0; i < nv; i++)
                {
                    if (child.LastStatus[i] == 0) WriteAnalogInput(b + 10u + (uint)i, data.Values[i]);
                }
            }
        }

        /// <summary>poem/&lt;pod&gt;/node/&lt;adr&gt;/diag: 稼働時間を反映する</summary>
        public void OnDiag(byte adr, NodeDiagPayload diag)
        {
            lock (syncLock)
            {
                if (!childrenByAddr.TryGetValue(adr, out ChildNode? child)) return;
                WriteAnalogInput(child.InstanceBase + 9u, diag.UptimeSec);
            }
        }

        #endregion

        #region BACnet オブジェクト生成 / 更新

        private void CreateBacnetObjects(ChildNode child)
        {
            uint b = child.InstanceBase;
            string disp = DisplayName(child);

            // オブジェクトは STATUS_FLAGS="0000" / RELIABILITY=0 で生成されるため、状態の追跡もリセット
            System.Array.Fill(child.LastStatus, 0u);

            AddDateTimeValue(b + 0u, $"{disp}_DT",    $"Last data publish time of {disp}", DateTime.Now);
            AddCharString  (b + 1u, $"{disp}_UID",   $"Device ID (16 bytes) of {disp}", child.Uid);
            AddCharString  (b + 2u, $"{disp}_NAME",  $"Configured label of {disp}", child.Name);
            AddAnalogInput (b + 3u, $"{disp}_FAULT", $"Faulty channel bits of {disp}", 0, BACNET_UNITS_NO_UNITS);
            AddAnalogInput (b + 4u, $"{disp}_SEQ",   $"Sample sequence of {disp}", 0, BACNET_UNITS_NO_UNITS);
            AddAnalogInput (b + 5u, $"{disp}_CNT",   $"Channel count of {disp}", child.Channels.Length, BACNET_UNITS_NO_UNITS);
            // base+6: 全チャネルが正常なデータを最後に受信した日時。base+0 が「生存信号」なのに対し、
            // こちらは「値がいつまで信頼できたか」を示す。生成時刻を初期値とする。
            AddDateTimeValue(b + 6u, $"{disp}_NDT",  $"Last normal data time of {disp}", DateTime.Now);
            AddCharString  (b + 7u, $"{disp}_MODEL", $"Model of {disp}", ModelLabel(child.Model));
            AddCharString  (b + 8u, $"{disp}_FW",    $"Firmware version of {disp}", child.Firmware);
            AddAnalogInput (b + 9u, $"{disp}_UPTIME", $"Uptime of {disp}", 0, BACNET_UNITS_SECONDS);

            for (int i = 0; i < child.Channels.Length; i++)
            {
                var ch = child.Channels[i];
                string chName = string.IsNullOrWhiteSpace(ch.Name) ? $"CH{i}" : ch.Name.Trim();
                AddAnalogInput(b + 10u + (uint)i,
                    $"{disp}_{chName}",
                    $"Channel {i} ({chName}) of {disp}",
                    0.0,
                    ch.Unit);
            }
        }

        /// <summary>子機 1 台分の BACnet オブジェクトをブロック単位 (base..base+99) で撤去する。</summary>
        private void RemoveBacnetObjects(ChildNode child)
        {
            Communicator.Storage.RemoveObjectsInRange(child.InstanceBase, child.InstanceBase + INSTANCE_BLOCK);
        }

        private void SetDeviceName(string name)
        {
            BacnetObjectId oid = new(BacnetObjectTypes.OBJECT_DEVICE, Communicator.Storage.DeviceId);
            Communicator.Storage.WriteProperty(oid, BacnetPropertyIds.PROP_OBJECT_NAME,
                new BacnetValue(BacnetApplicationTags.BACNET_APPLICATION_TAG_CHARACTER_STRING, name));
        }

        #endregion

        #region 低レベルヘルパ (オブジェクト追加 / プロパティ書込)

        /// <summary>
        /// DATETIME を Property.SerializeValue と同一の表現にする。
        /// 小数部は 1/100 秒 2 桁なので、ミリ秒を 10 で割って付加する。
        /// </summary>
        private static string FormatDateTime(DateTime value)
        {
            return value.ToString(DATE_TIME_FORMAT, System.Globalization.CultureInfo.InvariantCulture)
                 + (value.Millisecond / 10).ToString("D2", System.Globalization.CultureInfo.InvariantCulture);
        }

        // NuGet 版 Property は (id, tag, value) コンストラクタが無いので
        // オブジェクト初期化子で同等の構築を行う小さなラッパ。
        private static Property MakeProp(BacnetPropertyIds id, BacnetApplicationTags tag, string value)
        {
            return new Property { Id = id, Tag = tag, Value = new[] { value } };
        }

        private void AddDateTimeValue(uint instance, string name, string description, DateTime value)
        {
            Communicator.Storage.AddObject(new System.IO.BACnet.Storage.Object
            {
                Instance = instance,
                Type = BacnetObjectTypes.OBJECT_DATETIME_VALUE,
                Properties = new[]
                {
                    MakeProp(BacnetPropertyIds.PROP_OBJECT_IDENTIFIER, BacnetApplicationTags.BACNET_APPLICATION_TAG_OBJECT_ID, $"OBJECT_DATETIME_VALUE:{instance}"),
                    MakeProp(BacnetPropertyIds.PROP_OBJECT_NAME,        BacnetApplicationTags.BACNET_APPLICATION_TAG_CHARACTER_STRING, name),
                    MakeProp(BacnetPropertyIds.PROP_OBJECT_TYPE,        BacnetApplicationTags.BACNET_APPLICATION_TAG_ENUMERATED, "44"),
                    MakeProp(BacnetPropertyIds.PROP_DESCRIPTION,        BacnetApplicationTags.BACNET_APPLICATION_TAG_CHARACTER_STRING, description),
                    MakeProp(BacnetPropertyIds.PROP_PRESENT_VALUE,      BacnetApplicationTags.BACNET_APPLICATION_TAG_DATETIME, FormatDateTime(value)),
                    MakeProp(BacnetPropertyIds.PROP_STATUS_FLAGS,       BacnetApplicationTags.BACNET_APPLICATION_TAG_BIT_STRING, "0000"),
                }
            });
        }

        private void AddCharString(uint instance, string name, string description, string value)
        {
            Communicator.Storage.AddObject(new System.IO.BACnet.Storage.Object
            {
                Instance = instance,
                Type = BacnetObjectTypes.OBJECT_CHARACTERSTRING_VALUE,
                Properties = new[]
                {
                    MakeProp(BacnetPropertyIds.PROP_OBJECT_IDENTIFIER, BacnetApplicationTags.BACNET_APPLICATION_TAG_OBJECT_ID, $"OBJECT_CHARACTERSTRING_VALUE:{instance}"),
                    MakeProp(BacnetPropertyIds.PROP_OBJECT_NAME,        BacnetApplicationTags.BACNET_APPLICATION_TAG_CHARACTER_STRING, name),
                    MakeProp(BacnetPropertyIds.PROP_OBJECT_TYPE,        BacnetApplicationTags.BACNET_APPLICATION_TAG_ENUMERATED, "40"),
                    MakeProp(BacnetPropertyIds.PROP_DESCRIPTION,        BacnetApplicationTags.BACNET_APPLICATION_TAG_CHARACTER_STRING, description),
                    MakeProp(BacnetPropertyIds.PROP_PRESENT_VALUE,      BacnetApplicationTags.BACNET_APPLICATION_TAG_CHARACTER_STRING, value),
                    MakeProp(BacnetPropertyIds.PROP_STATUS_FLAGS,       BacnetApplicationTags.BACNET_APPLICATION_TAG_BIT_STRING, "0000"),
                }
            });
        }

        private void AddAnalogInput(uint instance, string name, string description, double value, ushort unit)
        {
            Communicator.Storage.AddObject(new System.IO.BACnet.Storage.Object
            {
                Instance = instance,
                Type = BacnetObjectTypes.OBJECT_ANALOG_INPUT,
                Properties = new[]
                {
                    MakeProp(BacnetPropertyIds.PROP_OBJECT_IDENTIFIER, BacnetApplicationTags.BACNET_APPLICATION_TAG_OBJECT_ID, $"OBJECT_ANALOG_INPUT:{instance}"),
                    MakeProp(BacnetPropertyIds.PROP_OBJECT_NAME,        BacnetApplicationTags.BACNET_APPLICATION_TAG_CHARACTER_STRING, name),
                    MakeProp(BacnetPropertyIds.PROP_OBJECT_TYPE,        BacnetApplicationTags.BACNET_APPLICATION_TAG_ENUMERATED, "0"),
                    MakeProp(BacnetPropertyIds.PROP_DESCRIPTION,        BacnetApplicationTags.BACNET_APPLICATION_TAG_CHARACTER_STRING, description),
                    MakeProp(BacnetPropertyIds.PROP_PRESENT_VALUE,      BacnetApplicationTags.BACNET_APPLICATION_TAG_REAL, value.ToString("R", System.Globalization.CultureInfo.InvariantCulture)),
                    MakeProp(BacnetPropertyIds.PROP_STATUS_FLAGS,       BacnetApplicationTags.BACNET_APPLICATION_TAG_BIT_STRING, "0000"),
                    MakeProp(BacnetPropertyIds.PROP_OUT_OF_SERVICE,     BacnetApplicationTags.BACNET_APPLICATION_TAG_BOOLEAN, "False"),
                    MakeProp(BacnetPropertyIds.PROP_RELIABILITY,        BacnetApplicationTags.BACNET_APPLICATION_TAG_ENUMERATED, "0"),
                    MakeProp(BacnetPropertyIds.PROP_UNITS,              BacnetApplicationTags.BACNET_APPLICATION_TAG_ENUMERATED, unit.ToString()),
                }
            });
        }

        /// <summary>
        /// チャネルの状態を反映する。reliability ≠ 0 なら STATUS_FLAGS の FAULT ビットを立て、
        /// RELIABILITY に子機が送ってきた値をそのまま書く (PRESENT_VALUE は最終有効値のまま)。
        /// </summary>
        private void WriteValueFault(uint instance, uint reliability)
        {
            BacnetObjectId oid = new(BacnetObjectTypes.OBJECT_ANALOG_INPUT, instance);
            bool fault = reliability != BACNET_RELIABILITY_NO_FAULT_DETECTED;

            // STATUS_FLAGS: {IN_ALARM, FAULT, OVERRIDDEN, OUT_OF_SERVICE}
            Communicator.Storage.WriteProperty(oid, BacnetPropertyIds.PROP_STATUS_FLAGS,
                new BacnetValue(BacnetApplicationTags.BACNET_APPLICATION_TAG_BIT_STRING,
                    BacnetBitString.Parse(fault ? "0100" : "0000")));

            Communicator.Storage.WriteProperty(oid, BacnetPropertyIds.PROP_RELIABILITY,
                new BacnetValue(BacnetApplicationTags.BACNET_APPLICATION_TAG_ENUMERATED, reliability));
        }

        private void WriteAnalogInput(uint instance, double value)
        {
            BacnetObjectId oid = new(BacnetObjectTypes.OBJECT_ANALOG_INPUT, instance);
            Communicator.Storage.WriteProperty(oid, BacnetPropertyIds.PROP_PRESENT_VALUE,
                new BacnetValue(BacnetApplicationTags.BACNET_APPLICATION_TAG_REAL, (float)value));
        }

        private void WriteCharString(uint instance, string value)
        {
            BacnetObjectId oid = new(BacnetObjectTypes.OBJECT_CHARACTERSTRING_VALUE, instance);
            Communicator.Storage.WriteProperty(oid, BacnetPropertyIds.PROP_PRESENT_VALUE,
                new BacnetValue(BacnetApplicationTags.BACNET_APPLICATION_TAG_CHARACTER_STRING, value));
        }

        private void WriteDateTime(uint instance, DateTime value)
        {
            BacnetObjectId oid = new(BacnetObjectTypes.OBJECT_DATETIME_VALUE, instance);
            Communicator.Storage.WriteProperty(oid, BacnetPropertyIds.PROP_PRESENT_VALUE,
                new BacnetValue(BacnetApplicationTags.BACNET_APPLICATION_TAG_DATETIME, value));
        }

        #endregion

        #region 内部ユーティリティ

        private static ChannelDesc[] SanitizeChannels(ChannelDesc[]? ch)
        {
            if (ch == null) return System.Array.Empty<ChannelDesc>();
            return ch.Take(MAX_VALUES_PER_NODE).ToArray();
        }

        private static string ModelLabel(ushort model)
        {
            return MODEL_NAMES.TryGetValue(model, out string? name)
                ? $"{name} 0x{model:X4}"
                : $"0x{model:X4}";
        }

        /// <summary>
        /// BACnet オブジェクト名のベース文字列。デバイス内で Object_Name の一意性が要求されるため、
        /// I²C アドレス由来のプレフィックス (A008..A119) を必ず付ける (子機の名前の重複対策)。
        /// </summary>
        private static string DisplayName(ChildNode child)
        {
            string n = child.Name?.Trim() ?? "";
            string label = string.IsNullOrEmpty(n)
                ? $"Node_{(child.Uid.Length >= 8 ? child.Uid[^8..] : child.Uid)}"
                : n;
            return $"A{child.I2cAddr:D3}_{label}";
        }

        #endregion

        /// <summary>子機 1 つの登録情報 (PoEMDevice 内部のみで使用)</summary>
        private sealed class ChildNode
        {
            public byte I2cAddr;
            public uint InstanceBase;
            public string Uid = "";
            public string Name = "";
            public ushort Model;
            public string Firmware = "";
            public ChannelDesc[] Channels = System.Array.Empty<ChannelDesc>();

            /// <summary>直近に BACnet へ反映したチャネルの状態 (FAULT/RELIABILITY の差分更新用)</summary>
            public uint[] LastStatus = new uint[MAX_VALUES_PER_NODE];

            public void Apply(NodeConfigPayload c, ChannelDesc[] channels)
            {
                Uid = c.Uid ?? "";
                Name = c.Name ?? "";
                Model = c.Model;
                Firmware = c.Firmware ?? "";
                Channels = channels;
            }

            /// <summary>オブジェクトの構成 (チャネル数・名前・単位) が同じか</summary>
            public bool SameLayout(ChannelDesc[] other)
            {
                if (other.Length != Channels.Length) return false;
                for (int i = 0; i < other.Length; i++)
                {
                    if (other[i].Name != Channels[i].Name || other[i].Unit != Channels[i].Unit) return false;
                }
                return true;
            }
        }
    }
}
