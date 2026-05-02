using System.IO.BACnet;
using System.IO.BACnet.Storage;
using BaCSharp;

namespace PoEMServer.BACnet
{
    /// <summary>
    /// PoEM 親機 (poem_pod) からの MQTT publish を BACnet オブジェクトへ反映する。
    ///
    /// 子機は親機の SCAN 時に "nodes/config/&lt;hex&gt;" で固定情報 (DataCount/UnitType/Name) を、
    /// 毎秒 "nodes/data/&lt;hex&gt;" で計測値を publish する。本クラスは:
    ///   - 初めて受信した DeviceID については BACnet オブジェクトを動的に生成 (BACnet 標準の
    ///     CreateObject 相当)。OBJECT_LIST はストレージから自動生成される。
    ///   - 既知の DeviceID については data 受信のたびに PRESENT_VALUE を更新。
    ///
    /// BACnet オブジェクト割当 (各子機 ordinal idx に対し base = 100 + idx*100):
    ///   base+0   DateTimeValue    最終受信日時
    ///   base+1   CharStringValue  DeviceID HEX
    ///   base+2   CharStringValue  Name (子機 EEPROM 由来)
    ///   base+3   AnalogInput      Status1 (生 bitmask)
    ///   base+4   AnalogInput      Seq (シーケンス番号)
    ///   base+5   AnalogInput      DataCount
    ///   base+10..base+17  AnalogInput  value[0..7] (UNITS は config の u[i] から)
    /// </summary>
    internal class PoEMDevice
    {
        #region 定数

        private const string DATE_TIME_FORMAT = "yyyy-MM-ddTHH:mm:ss.fff";

        /// <summary>子機 1 つあたりの BACnet Instance 番号確保枠</summary>
        private const uint INSTANCE_BLOCK = 100;

        /// <summary>1 つ目の子機の base 番号 (0..99 はデバイス自身用に予約)</summary>
        private const uint FIRST_INSTANCE_BASE = 100;

        /// <summary>1 子機が公開できる計測値の上限 (firmware 仕様と同じ)</summary>
        private const int MAX_VALUES_PER_NODE = 8;

        #endregion

        #region インスタンス変数

        public BACnetCommunicator Communicator { get; private set; }

        /// <summary>DeviceID → ChildNode の登録表</summary>
        private readonly Dictionary<uint, ChildNode> children = new();

        /// <summary>次に新規子機へ割り当てる base instance</summary>
        private uint nextInstanceBase = FIRST_INSTANCE_BASE;

        /// <summary>storage / children を保護するロック</summary>
        private readonly object syncLock = new();

        #endregion

        #region コンストラクタ

        public PoEMDevice(int exclusivePort, string localEndPointAddress)
        {
            string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "PoEMDeviceStorage.xml");
            DeviceStorage storage = DeviceStorage.Load(path);
            Communicator = new BACnetCommunicator(storage, exclusivePort, localEndPointAddress);
        }

        #endregion

        #region 公開メソッド

        /// <summary>
        /// "nodes/config/&lt;hex&gt;" 受信時の処理。新規 DeviceID なら BACnet オブジェクトを
        /// 生成、既知なら Name / UnitType を更新する。
        /// </summary>
        public void OnConfig(NodeConfigPayload config)
        {
            if (!TryParseDeviceId(config.IdHex, out uint deviceId)) return;

            lock (syncLock)
            {
                if (!children.TryGetValue(deviceId, out ChildNode? child))
                {
                    child = new ChildNode
                    {
                        DeviceId = deviceId,
                        InstanceBase = nextInstanceBase,
                        Name = config.Name ?? "",
                        DataCount = Math.Clamp(config.DataCount, 0, MAX_VALUES_PER_NODE),
                        UnitTypes = SanitizeUnits(config.UnitTypes),
                    };
                    nextInstanceBase += INSTANCE_BLOCK;
                    children[deviceId] = child;

                    CreateBacnetObjects(child);
                }
                else
                {
                    // 既存子機の config 再受信: Name / UnitType の変更を反映
                    string newName = config.Name ?? "";
                    int newCnt = Math.Clamp(config.DataCount, 0, MAX_VALUES_PER_NODE);
                    ushort[] newUnits = SanitizeUnits(config.UnitTypes);

                    if (newName != child.Name)
                    {
                        child.Name = newName;
                        WriteCharString(child.InstanceBase + 2u, DisplayName(child));
                    }
                    // DataCount や UnitType の変更はオブジェクト追加が必要なケースがあるが、
                    // 本実装ではアプリ再起動で再ビルドする前提で、ここでは PRESENT_VALUE のみ更新
                    if (newCnt != child.DataCount)
                    {
                        child.DataCount = newCnt;
                        WriteAnalogInput(child.InstanceBase + 5u, newCnt);
                    }
                    if (!UnitsEqual(child.UnitTypes, newUnits))
                    {
                        child.UnitTypes = newUnits;
                        UpdateValueUnits(child);
                    }
                }
            }
        }

        /// <summary>
        /// "nodes/data/&lt;hex&gt;" 受信時の処理。config 未受信の DeviceID は無視 (config 待ち)。
        /// </summary>
        public void OnData(NodeDataPayload data)
        {
            if (!TryParseDeviceId(data.IdHex, out uint deviceId)) return;

            lock (syncLock)
            {
                if (!children.TryGetValue(deviceId, out ChildNode? child)) return;

                uint b = child.InstanceBase;

                WriteDateTime(b + 0u, DateTime.Now);
                WriteAnalogInput(b + 3u, data.Status1);
                WriteAnalogInput(b + 4u, data.Seq);

                int n = Math.Min(child.DataCount, data.Values?.Length ?? 0);
                for (int i = 0; i < n; i++)
                {
                    WriteAnalogInput(b + 10u + (uint)i, data.Values![i]);
                }
            }
        }

        #endregion

        #region BACnet オブジェクト生成 / 更新

        private void CreateBacnetObjects(ChildNode child)
        {
            uint b = child.InstanceBase;
            string disp = DisplayName(child);

            AddDateTimeValue(b + 0u, $"{disp}_DT", $"Last data publish time of {disp}", DateTime.Now);
            AddCharString  (b + 1u, $"{disp}_ID", $"DeviceID (FNV-1a hash) of {disp}", $"{child.DeviceId:X8}");
            AddCharString  (b + 2u, $"{disp}_NAME", $"Configured label of {disp}", child.Name);
            AddAnalogInput (b + 3u, $"{disp}_ST",  $"Status1 bitmask of {disp}", 0, BACNET_UNITS_NO_UNITS);
            AddAnalogInput (b + 4u, $"{disp}_SEQ", $"Sequence number of {disp}", 0, BACNET_UNITS_NO_UNITS);
            AddAnalogInput (b + 5u, $"{disp}_CNT", $"Active value count of {disp}", child.DataCount, BACNET_UNITS_NO_UNITS);

            for (int i = 0; i < child.DataCount; i++)
            {
                ushort unit = child.UnitTypes.Length > i ? child.UnitTypes[i] : BACNET_UNITS_NO_UNITS;
                AddAnalogInput(b + 10u + (uint)i,
                    $"{disp}_V{i}",
                    $"value[{i}] of {disp}",
                    0.0,
                    unit);
            }
        }

        private void UpdateValueUnits(ChildNode child)
        {
            uint b = child.InstanceBase;
            for (int i = 0; i < child.DataCount; i++)
            {
                ushort unit = child.UnitTypes.Length > i ? child.UnitTypes[i] : BACNET_UNITS_NO_UNITS;
                BacnetObjectId oid = new(BacnetObjectTypes.OBJECT_ANALOG_INPUT, b + 10u + (uint)i);
                Communicator.Storage.WriteProperty(oid, BacnetPropertyIds.PROP_UNITS,
                    new BacnetValue(BacnetApplicationTags.BACNET_APPLICATION_TAG_ENUMERATED, (uint)unit));
            }
        }

        #endregion

        #region 低レベルヘルパ (オブジェクト追加 / プロパティ書込)

        private const ushort BACNET_UNITS_NO_UNITS = 95;

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
                    MakeProp(BacnetPropertyIds.PROP_PRESENT_VALUE,      BacnetApplicationTags.BACNET_APPLICATION_TAG_DATETIME, value.ToString(DATE_TIME_FORMAT)),
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

        private static bool TryParseDeviceId(string hex, out uint id)
        {
            return uint.TryParse(hex, System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture, out id);
        }

        private static ushort[] SanitizeUnits(ushort[] u)
        {
            if (u == null) return System.Array.Empty<ushort>();
            int n = Math.Min(u.Length, MAX_VALUES_PER_NODE);
            ushort[] arr = new ushort[n];
            System.Array.Copy(u, arr, n);
            return arr;
        }

        private static bool UnitsEqual(ushort[] a, ushort[] b)
        {
            if (a.Length != b.Length) return false;
            for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false;
            return true;
        }

        /// <summary>BACnet オブジェクト名のベース文字列。Name が空なら DeviceID。</summary>
        private static string DisplayName(ChildNode child)
        {
            string n = child.Name?.Trim() ?? "";
            return string.IsNullOrEmpty(n) ? $"Node_{child.DeviceId:X8}" : n;
        }

        #endregion

        /// <summary>子機 1 つの登録情報 (PoEMDevice 内部のみで使用)</summary>
        private sealed class ChildNode
        {
            public uint DeviceId;
            public uint InstanceBase;
            public string Name = "";
            public int DataCount;
            public ushort[] UnitTypes = System.Array.Empty<ushort>();
        }
    }
}
