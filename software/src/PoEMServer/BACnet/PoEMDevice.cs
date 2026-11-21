using System.IO.BACnet;
using System.IO.BACnet.Storage;
using BaCSharp;
using PoEM.Common;

namespace PoEMServer.BACnet
{
    /// <summary>
    /// PoEMServer の BACnet Device (サーバ 1 台につき 1 つ)。MQTT v2 のメッセージを BACnet オブジェクトへ反映する。
    ///
    /// Device インスタンス番号とポートはサーバの設定で決める (既定はポート 47808、番号は bacip の末尾)。
    /// 親機はポートでは分けず、オブジェクトのインスタンス番号の範囲で分ける:
    ///   親機の範囲 pbase = 親機の番号 × 12000 (番号は PodRegistry が振り、PodMap.json に保存する)
    ///   子機の範囲 base  = pbase + I²C アドレス × 100 (アドレス 8..119 → pbase+800..pbase+11999)
    /// 番号の上限 (4194302) から、親機は 349 台まで扱える。
    /// 管理単位は「親機内のアドレス = 物理配置」。センサを交換して同じアドレスに別個体を付けても
    /// インスタンス番号は変わらず、Device ID がプロパティとして更新される。
    ///
    /// 親機のオブジェクト (pbase + 0..3):
    ///   pbase+0  CharStringValue  親機名
    ///   pbase+1  CharStringValue  親機 ID (MAC アドレス 12 桁)
    ///   pbase+2  CharStringValue  親機のファームウェアの版
    ///   pbase+3  CharStringValue  親機の IP アドレス
    ///
    /// 子機のオブジェクト (base = pbase + adr × 100):
    ///   base+0   DateTimeValue    最終受信日時
    ///   base+1   CharStringValue  表示用 ID (16 B の UID の FNV-1a 22bit に 0x100000 を足した 16 進 6 桁。説明に UID を記す)
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

        /// <summary>親機 1 台あたりのインスタンス番号の範囲 (pbase = 親機の番号 × この値)</summary>
        public const uint POD_BLOCK = 12000;

        /// <summary>子機 1 台あたりのインスタンス番号の範囲 (base = pbase + I²C アドレス × この値)</summary>
        private const uint INSTANCE_BLOCK = 100;

        /// <summary>1 子機が公開できるチャネルの上限 (OSL 共通レジスタ v2 と同じ)</summary>
        private const int MAX_VALUES_PER_NODE = 16;

        /// <summary>I²C アドレスの有効範囲 (firmware の走査範囲と同じ)</summary>
        private const byte I2C_ADDR_MIN = 0x08;
        private const byte I2C_ADDR_MAX = 0x77;

        private const ushort BACNET_UNITS_NO_UNITS = 95;
        private const ushort BACNET_UNITS_SECONDS  = 73;
        private const uint   BACNET_RELIABILITY_NO_FAULT_DETECTED = 0;

        #endregion

        #region インスタンス変数

        public BACnetCommunicator Communicator { get; private set; }

        /// <summary>親機の番号 (PodMap.json) を振る</summary>
        private readonly PodRegistry registry;

        /// <summary>親機 ID → 親機の登録情報</summary>
        private readonly Dictionary<string, PodNode> pods = new();

        /// <summary>storage / pods を保護するロック</summary>
        private readonly object syncLock = new();

        #endregion

        #region コンストラクタ

        /// <param name="templatePath">Device オブジェクトのひな形 (PoEMDeviceStorage.xml)</param>
        /// <param name="deviceInstance">BACnet Device インスタンス番号</param>
        /// <param name="deviceName">BACnet Device の名前</param>
        /// <param name="port">BACnet/IP の UDP ポート (既定 47808)</param>
        /// <param name="localEndPointAddress">BACnet/IP で使う自分の IP アドレス</param>
        /// <param name="registry">親機の番号の対応 (PodMap.json)</param>
        public PoEMDevice(string templatePath, uint deviceInstance, string deviceName, int port, string localEndPointAddress, PodRegistry registry)
        {
            this.registry = registry;
            DeviceStorage storage = DeviceStorage.Load(templatePath, deviceInstance);
            Communicator = new BACnetCommunicator(storage, port, localEndPointAddress);
            SetDeviceName(deviceName);
        }

        #endregion

        #region 公開メソッド

        /// <summary>poem/&lt;pod&gt;/pod/info: 親機の名前・版・IP を反映する</summary>
        public void OnPodInfo(string podId, PodInfoPayload info)
        {
            lock (syncLock)
            {
                var pod = GetPod(podId);
                string name = string.IsNullOrWhiteSpace(info.Name) ? podId : info.Name.Trim();
                WriteCharString(pod.Base + 0u, name);
                WriteCharString(pod.Base + 2u, info.Firmware ?? "");
                WriteCharString(pod.Base + 3u, info.Ip ?? "");
            }
        }

        /// <summary>
        /// poem/&lt;pod&gt;/node/&lt;adr&gt;/config: 新規アドレスならオブジェクトを生成し、既知なら変更を反映する。
        /// </summary>
        public void OnConfig(string podId, byte adr, NodeConfigPayload config)
        {
            if (adr < I2C_ADDR_MIN || adr > I2C_ADDR_MAX) return;
            lock (syncLock)
            {
                var pod = GetPod(podId);
                var channels = SanitizeChannels(config.Channels);

                // 同じ UID が別アドレスで登録済み → 子機のアドレス変更。旧ブロックを撤去する。
                var moved = pod.Children.Values.FirstOrDefault(c => c.Uid == config.Uid && c.I2cAddr != adr);
                if (moved != null)
                {
                    Console.WriteLine($"[INFO] pod {podId}: uid {config.Uid} moved from I2C address {moved.I2cAddr} to {adr}. Rebuilding BACnet objects.");
                    RemoveBacnetObjects(moved);
                    pod.Children.Remove(moved.I2cAddr);
                }

                if (!pod.Children.TryGetValue(adr, out ChildNode? child))
                {
                    child = new ChildNode { Pod = pod, I2cAddr = adr, InstanceBase = pod.Base + adr * INSTANCE_BLOCK };
                    child.Apply(config, channels);
                    pod.Children[adr] = child;
                    CreateBacnetObjects(child);
                    return;
                }

                // 同じアドレスに別個体 (センサ交換) または構成の変化 → オブジェクトを作り直す。
                // インスタンス番号はアドレス基準なので変わらない。
                if (child.Uid != config.Uid || !child.SameLayout(channels))
                {
                    if (child.Uid != config.Uid)
                        Console.WriteLine($"[INFO] pod {podId}: I2C address {adr}: device replaced ({child.Uid} -> {config.Uid}). Rebuilding BACnet objects.");
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
        public void OnData(string podId, byte adr, NodeDataPayload data)
        {
            lock (syncLock)
            {
                if (!pods.TryGetValue(podId, out PodNode? pod) || !pod.Children.TryGetValue(adr, out ChildNode? child)) return;

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
        public void OnDiag(string podId, byte adr, NodeDiagPayload diag)
        {
            lock (syncLock)
            {
                if (!pods.TryGetValue(podId, out PodNode? pod) || !pod.Children.TryGetValue(adr, out ChildNode? child)) return;
                WriteAnalogInput(child.InstanceBase + 9u, diag.UptimeSec);
            }
        }

        #endregion

        #region BACnet オブジェクト生成 / 更新

        /// <summary>親機の登録情報を返す。初めての親機なら番号を振り、親機のオブジェクトを作る</summary>
        private PodNode GetPod(string podId)
        {
            if (pods.TryGetValue(podId, out PodNode? pod)) return pod;

            int index = registry.IndexOf(podId);
            pod = new PodNode { Id = podId, Index = index, Base = (uint)index * POD_BLOCK };
            pods[podId] = pod;

            string p = PodPrefix(pod);
            AddCharString(pod.Base + 0u, $"{p}_POD_NAME", $"Name of pod {podId}", podId);
            AddCharString(pod.Base + 1u, $"{p}_POD_ID",   $"MAC address of pod {podId}", podId);
            AddCharString(pod.Base + 2u, $"{p}_POD_FW",   $"Firmware version of pod {podId}", "");
            AddCharString(pod.Base + 3u, $"{p}_POD_IP",   $"IP address of pod {podId}", "");
            Console.WriteLine($"[INFO] pod {podId}: index {index}, BACnet instances {pod.Base}..{pod.Base + POD_BLOCK - 1}.");
            return pod;
        }

        private void CreateBacnetObjects(ChildNode child)
        {
            uint b = child.InstanceBase;
            string disp = DisplayName(child);

            // オブジェクトは STATUS_FLAGS="0000" / RELIABILITY=0 で生成されるため、状態の追跡もリセット
            System.Array.Fill(child.LastStatus, 0u);

            AddDateTimeValue(b + 0u, $"{disp}_DT",    $"Last data publish time of {disp}", DateTime.Now);
            AddCharString  (b + 1u, $"{disp}_ID",    $"Display ID (FNV-1a 22bit of UID {child.Uid}, + 0x100000 in hex) of {disp}", DeviceId.DisplayFromUid(child.Uid));
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

        /// <summary>機種の表示 (名前と model_id)。名前の表は PoEM.Common にある</summary>
        private static string ModelLabel(ushort model)
            => OslTables.TryGetModelName(model, out string name) ? $"{name} 0x{model:X4}" : $"0x{model:X4}";

        /// <summary>親機を表すオブジェクト名の接頭辞 (P00, P01, …)</summary>
        private static string PodPrefix(PodNode pod) => $"P{pod.Index:D2}";

        /// <summary>
        /// BACnet オブジェクト名のベース文字列。デバイス内で Object_Name の一意性が要求されるため、
        /// 親機の番号と I²C アドレス由来の接頭辞 (P00_A016 など) を必ず付ける (子機の名前の重複対策)。
        /// </summary>
        private static string DisplayName(ChildNode child)
        {
            string n = child.Name?.Trim() ?? "";
            string label = string.IsNullOrEmpty(n) ? $"Node_{DeviceId.DisplayFromUid(child.Uid)}" : n;
            return $"{PodPrefix(child.Pod)}_A{child.I2cAddr:D3}_{label}";
        }

        #endregion

        /// <summary>親機 1 台の登録情報</summary>
        private sealed class PodNode
        {
            public string Id = "";
            public int Index;
            public uint Base;

            /// <summary>I²C アドレス → 子機</summary>
            public readonly Dictionary<byte, ChildNode> Children = new();
        }

        /// <summary>子機 1 つの登録情報 (PoEMDevice 内部のみで使用)</summary>
        private sealed class ChildNode
        {
            public PodNode Pod = null!;
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
