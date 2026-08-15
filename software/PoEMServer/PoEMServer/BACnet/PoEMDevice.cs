using System.IO.BACnet;
using System.IO.BACnet.Storage;
using BaCSharp;

namespace PoEMServer.BACnet
{
    /// <summary>
    /// PoEM 親機 (poem_pod) からの MQTT publish を BACnet オブジェクトへ反映する。
    ///
    /// 子機は親機の SCAN 時に "nodes/config/&lt;id&gt;" で固定情報 (I2cAddr/DataCount/UnitType/Name) を、
    /// 毎秒 "nodes/data/&lt;id&gt;" で計測値を publish する。本クラスは:
    ///   - 初めて受信した I²C アドレスについては BACnet オブジェクトを動的に生成。
    ///     OBJECT_LIST はストレージから自動生成される。
    ///   - 既知の子機については data 受信のたびに PRESENT_VALUE を更新。
    ///
    /// BACnet インスタンス番号は子機の I²C アドレスから固定的に決まる (base = adr × 100):
    ///   - アドレス範囲 8..119 → base 800..11900。0..99 はデバイス自身用に予約。
    ///   - 管理単位は「アドレス = 物理配置」。センサを故障交換して同アドレスに別個体を
    ///     付けてもインスタンス番号は変わらず、DeviceID はプロパティとして更新される。
    ///   - 子機のアドレスを変更した場合は旧 base のオブジェクトを撤去し、新 base に再生成する。
    ///
    /// フォールバック (レガシー方式): adr フィールドを持たない旧ファーム親機の config に対しては、
    /// DeviceID → InstanceBase の対応テーブルで 12000 番台から 100 刻みに発番する。対応は
    /// LegacyInstanceMap.json (実行ファイルと同じフォルダ) に永続化され、サーバ再起動後も
    /// 同じ DeviceID には同じ番号が割り当てられる。同じ DeviceID が後に adr 付き config で
    /// 現れた場合 (親機のファーム更新) は adr 方式へ移行し、レガシー割当は破棄される。
    ///
    /// オブジェクト割当 (base = adr × 100):
    ///   base+0   DateTimeValue    最終受信日時
    ///   base+1   CharStringValue  DeviceID (FNV-1a 22bit、個体識別)
    ///   base+2   CharStringValue  Name (子機 EEPROM 由来)
    ///   base+3   AnalogInput      Status1 (生 bitmask)
    ///   base+4   AnalogInput      Seq (子機が数えた計測回数)
    ///   base+5   AnalogInput      DataCount
    ///   base+6   DateTimeValue    最終正常データ受信日時 (status1==0x00 のときのみ更新)
    ///   base+10..base+17  AnalogInput  value[0..7] (UNITS は config の u[i] から)
    ///
    /// 子機が I²C 不達になると親機は最終値を stale フラグ (status1 の該当 bit) 付きで
    /// publish し続ける。本クラスはこれを BACnet の作法へ写す: 該当 value オブジェクトの
    /// PRESENT_VALUE は最終有効値のまま凍結し、STATUS_FLAGS の FAULT ビットと
    /// RELIABILITY (COMMUNICATION_FAILURE) で「値が信頼できない」ことを示す。
    /// </summary>
    internal class PoEMDevice
    {
        #region 定数

        /// <summary>
        /// DATETIME を文字列で持つときの書式。ライブラリ (System.IO.BACnet.Storage.Property) が
        /// ParseExact("yyyy/MM/dd-HH:mm:ss.ff") で読むため、これに一致させる必要がある
        /// (bacnet-stack 互換。小数部はミリ秒ではなく 1/100 秒の 2 桁)。
        /// 一致しないと解析に失敗し、base64 フォールバックも外れて PRESENT_VALUE が null になる。
        /// 小数部は <see cref="FormatDateTime"/> で付加する。
        /// </summary>
        private const string DATE_TIME_FORMAT = "yyyy/MM/dd-HH:mm:ss.";

        /// <summary>子機 1 つあたりの BACnet Instance 番号確保枠 (base = I²Cアドレス × この値)</summary>
        private const uint INSTANCE_BLOCK = 100;

        /// <summary>1 子機が公開できる計測値の上限 (firmware 仕様と同じ)</summary>
        private const int MAX_VALUES_PER_NODE = 8;

        /// <summary>I²C アドレスの有効範囲 (firmware の走査範囲と同じ)</summary>
        private const byte I2C_ADDR_MIN = 0x08;
        private const byte I2C_ADDR_MAX = 0x77;

        /// <summary>
        /// レガシー (adr 無し config) 子機の最初の base 番号。adr 方式の最大値
        /// (119×100+99 = 11999) と重ならないよう 12000 から 100 刻みに発番する。
        /// </summary>
        private const uint LEGACY_FIRST_INSTANCE_BASE = 12000;

        /// <summary>レガシー子機の DeviceID → InstanceBase 対応を永続化するファイル名</summary>
        private const string LEGACY_MAP_FILE = "LegacyInstanceMap.json";

        #endregion

        #region インスタンス変数

        public BACnetCommunicator Communicator { get; private set; }

        /// <summary>I²C アドレス → ChildNode の登録表 (adr 方式の主キー)</summary>
        private readonly Dictionary<byte, ChildNode> childrenByAddr = new();

        /// <summary>DeviceID → ChildNode (data 受信時の引き当て。adr 方式・レガシー方式の両方が入る)</summary>
        private readonly Dictionary<uint, ChildNode> childrenById = new();

        /// <summary>レガシー子機の DeviceID → InstanceBase 対応表 (LegacyInstanceMap.json に永続化)</summary>
        private readonly Dictionary<uint, uint> legacyBases = new();

        /// <summary>次にレガシー子機へ割り当てる base instance</summary>
        private uint nextLegacyBase = LEGACY_FIRST_INSTANCE_BASE;

        /// <summary>レガシー対応表の保存先パス</summary>
        private readonly string legacyMapPath;

        /// <summary>storage / children を保護するロック</summary>
        private readonly object syncLock = new();

        #endregion

        #region コンストラクタ

        public PoEMDevice(int exclusivePort, string localEndPointAddress)
        {
            string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "PoEMDeviceStorage.xml");
            DeviceStorage storage = DeviceStorage.Load(path);
            Communicator = new BACnetCommunicator(storage, exclusivePort, localEndPointAddress);

            legacyMapPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, LEGACY_MAP_FILE);
            LoadLegacyMap();
        }

        #endregion

        #region 公開メソッド

        /// <summary>
        /// "nodes/config/&lt;id&gt;" 受信時の処理。adr の有無で adr 方式 / レガシー方式に振り分ける。
        /// </summary>
        public void OnConfig(NodeConfigPayload config)
        {
            lock (syncLock)
            {
                if (config.I2cAddr < I2C_ADDR_MIN || config.I2cAddr > I2C_ADDR_MAX)
                    OnConfigLegacy(config);     // adr フィールドを持たない旧ファーム
                else
                    OnConfigAddressed(config);  // adr 対応ファーム (base = adr×100)
            }
        }

        /// <summary>adr 対応ファームの config 処理。新規アドレスならオブジェクト生成、既知なら変更を反映。</summary>
        private void OnConfigAddressed(NodeConfigPayload config)
        {
            byte adr = config.I2cAddr;

            // この DeviceID がレガシー方式で登録済み → 親機のファーム更新。adr 方式へ移行する。
            if (childrenById.TryGetValue(config.Id, out ChildNode? prev) && prev.I2cAddr == 0)
            {
                Console.WriteLine($"[INFO] id={config.Id}: firmware now reports I2C address {adr}. Migrating from legacy instance {prev.InstanceBase} to {adr * INSTANCE_BLOCK}.");
                RemoveBacnetObjects(prev);
                childrenById.Remove(config.Id);
                if (legacyBases.Remove(config.Id)) SaveLegacyMap();
                prev = null;
            }

            // 同じ DeviceID が別アドレスで登録済み → 子機のアドレス変更。旧ブロックを撤去して再生成する。
            if (prev != null && prev.I2cAddr != adr)
            {
                Console.WriteLine($"[INFO] id={config.Id} moved from I2C address {prev.I2cAddr} to {adr}. Rebuilding BACnet objects.");
                RemoveBacnetObjects(prev);
                childrenByAddr.Remove(prev.I2cAddr);
                childrenById.Remove(config.Id);
            }

            if (!childrenByAddr.TryGetValue(adr, out ChildNode? child))
            {
                child = new ChildNode
                {
                    I2cAddr = adr,
                    DeviceId = config.Id,
                    InstanceBase = adr * INSTANCE_BLOCK,
                    Name = config.Name ?? "",
                    DataCount = Math.Clamp(config.DataCount, 0, MAX_VALUES_PER_NODE),
                    UnitTypes = SanitizeUnits(config.UnitTypes),
                };
                childrenByAddr[adr] = child;
                childrenById[config.Id] = child;

                CreateBacnetObjects(child);
                return;
            }

            // 既知アドレスの config 再受信
            string newName = config.Name ?? "";
            int newCnt = Math.Clamp(config.DataCount, 0, MAX_VALUES_PER_NODE);
            ushort[] newUnits = SanitizeUnits(config.UnitTypes);

            // 同アドレスに別個体 (センサ交換) または計測値数の変更 → オブジェクト構成が
            // 変わりうるのでブロックごと再生成する。インスタンス番号はアドレス基準なので不変。
            if (child.DeviceId != config.Id || newCnt != child.DataCount)
            {
                if (child.DeviceId != config.Id)
                    Console.WriteLine($"[INFO] I2C address {adr}: device replaced (id {child.DeviceId} -> {config.Id}). Rebuilding BACnet objects.");

                RemoveBacnetObjects(child);
                childrenById.Remove(child.DeviceId);

                child.DeviceId = config.Id;
                child.Name = newName;
                child.DataCount = newCnt;
                child.UnitTypes = newUnits;
                childrenById[config.Id] = child;

                CreateBacnetObjects(child);
                return;
            }

            UpdateNameAndUnits(child, newName, newUnits);
        }

        /// <summary>
        /// レガシー (adr 無し) config の処理。DeviceID をキーに、永続対応表 (LegacyInstanceMap.json)
        /// から InstanceBase を引き当てる。未知の DeviceID には 12000 番台から順に発番して保存する。
        /// </summary>
        private void OnConfigLegacy(NodeConfigPayload config)
        {
            string newName = config.Name ?? "";
            int newCnt = Math.Clamp(config.DataCount, 0, MAX_VALUES_PER_NODE);
            ushort[] newUnits = SanitizeUnits(config.UnitTypes);

            if (childrenById.TryGetValue(config.Id, out ChildNode? child))
            {
                if (child.I2cAddr != 0) return;  // adr 方式で登録済みの個体からの旧形式 config は無視 (通常あり得ない)

                // 計測値数の変更はオブジェクト構成が変わるのでブロックごと再生成 (番号は不変)
                if (newCnt != child.DataCount)
                {
                    RemoveBacnetObjects(child);
                    child.Name = newName;
                    child.DataCount = newCnt;
                    child.UnitTypes = newUnits;
                    CreateBacnetObjects(child);
                    return;
                }

                UpdateNameAndUnits(child, newName, newUnits);
                return;
            }

            // 新規レガシー子機: 永続対応表から番号を引く。無ければ発番して保存。
            if (!legacyBases.TryGetValue(config.Id, out uint instanceBase))
            {
                instanceBase = nextLegacyBase;
                nextLegacyBase += INSTANCE_BLOCK;
                legacyBases[config.Id] = instanceBase;
                SaveLegacyMap();
                Console.WriteLine($"[INFO] legacy config (no adr) for id={config.Id}: assigned instance base {instanceBase} (persisted).");
            }

            child = new ChildNode
            {
                I2cAddr = 0,   // 0 = レガシー (アドレス不明)
                DeviceId = config.Id,
                InstanceBase = instanceBase,
                Name = newName,
                DataCount = newCnt,
                UnitTypes = newUnits,
            };
            childrenById[config.Id] = child;

            CreateBacnetObjects(child);
        }

        /// <summary>Name / UnitType の変更だけを既存オブジェクトへ反映する (構成不変の場合)。</summary>
        private void UpdateNameAndUnits(ChildNode child, string newName, ushort[] newUnits)
        {
            if (newName != child.Name)
            {
                child.Name = newName;
                WriteCharString(child.InstanceBase + 2u, child.Name);
            }
            if (!UnitsEqual(child.UnitTypes, newUnits))
            {
                child.UnitTypes = newUnits;
                UpdateValueUnits(child);
            }
        }

        /// <summary>
        /// "nodes/data/&lt;id&gt;" 受信時の処理。config 未受信の DeviceID は無視 (config 待ち)。
        /// </summary>
        public void OnData(NodeDataPayload data)
        {
            lock (syncLock)
            {
                if (!childrenById.TryGetValue(data.Id, out ChildNode? child)) return;

                uint b = child.InstanceBase;

                WriteDateTime(b + 0u, DateTime.Now);
                if (data.Status1 == 0x00)
                {
                    // 全計測値が有効なデータのみ「最終正常データ受信日時」を進める
                    WriteDateTime(b + 6u, DateTime.Now);
                }
                WriteAnalogInput(b + 3u, data.Status1);
                WriteAnalogInput(b + 4u, data.Seq);

                int n = Math.Min(child.DataCount, data.Values?.Length ?? 0);
                for (int i = 0; i < n; i++)
                {
                    WriteAnalogInput(b + 10u + (uint)i, data.Values![i]);
                }

                // status1 (per-value の stale bitmask) の変化分だけ FAULT / RELIABILITY を更新する。
                // 毎秒書き換えると COV 通知やストレージ書込が無駄に走るため、変化時のみ。
                if (data.Status1 != child.LastStatus1)
                {
                    for (int i = 0; i < child.DataCount; i++)
                    {
                        bool wasFault = ((child.LastStatus1 >> i) & 1) != 0;
                        bool isFault = ((data.Status1 >> i) & 1) != 0;
                        if (wasFault != isFault)
                        {
                            WriteValueFault(b + 10u + (uint)i, isFault);
                        }
                    }
                    child.LastStatus1 = data.Status1;
                }
            }
        }

        #endregion

        #region BACnet オブジェクト生成 / 更新

        private void CreateBacnetObjects(ChildNode child)
        {
            uint b = child.InstanceBase;
            string disp = DisplayName(child);

            // オブジェクトは STATUS_FLAGS="0000" / RELIABILITY=0 で生成されるため、フォルト追跡もリセット
            child.LastStatus1 = 0x00;

            AddDateTimeValue(b + 0u, $"{disp}_DT", $"Last data publish time of {disp}", DateTime.Now);
            AddCharString  (b + 1u, $"{disp}_ID", $"DeviceID (FNV-1a 22bit) of {disp}", $"{child.DeviceId}");
            AddCharString  (b + 2u, $"{disp}_NAME", $"Configured label of {disp}", child.Name);
            AddAnalogInput (b + 3u, $"{disp}_ST",  $"Status1 bitmask of {disp}", 0, BACNET_UNITS_NO_UNITS);
            AddAnalogInput (b + 4u, $"{disp}_SEQ", $"Measurement count of {disp}", 0, BACNET_UNITS_NO_UNITS);
            AddAnalogInput (b + 5u, $"{disp}_CNT", $"Active value count of {disp}", child.DataCount, BACNET_UNITS_NO_UNITS);
            // base+6: 全計測値が有効 (status1==0x00) なデータを最後に受信した日時。
            // base+0 が「生存信号」なのに対し、こちらは「値がいつまで信頼できたか」を示す。
            // 生成時点では正常データ未受信だが、便宜上生成時刻を初期値とする (base+0 と同じ扱い)。
            AddDateTimeValue(b + 6u, $"{disp}_NDT", $"Last normal data (status1==0) time of {disp}", DateTime.Now);

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

        /// <summary>子機 1 台分の BACnet オブジェクトをブロック単位 (base..base+99) で撤去する。</summary>
        private void RemoveBacnetObjects(ChildNode child)
        {
            Communicator.Storage.RemoveObjectsInRange(child.InstanceBase, child.InstanceBase + INSTANCE_BLOCK);
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

        /// <summary>BACnet Reliability 列挙値 (ASHRAE 135)</summary>
        private const uint BACNET_RELIABILITY_NO_FAULT_DETECTED = 0;
        private const uint BACNET_RELIABILITY_COMMUNICATION_FAILURE = 12;

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
        /// 計測値 AI オブジェクトのフォルト状態を設定する。
        /// fault=true で STATUS_FLAGS の FAULT ビットを立て、RELIABILITY を
        /// COMMUNICATION_FAILURE にする (PRESENT_VALUE は最終有効値のまま)。
        /// </summary>
        private void WriteValueFault(uint instance, bool fault)
        {
            BacnetObjectId oid = new(BacnetObjectTypes.OBJECT_ANALOG_INPUT, instance);

            // STATUS_FLAGS: {IN_ALARM, FAULT, OVERRIDDEN, OUT_OF_SERVICE}
            Communicator.Storage.WriteProperty(oid, BacnetPropertyIds.PROP_STATUS_FLAGS,
                new BacnetValue(BacnetApplicationTags.BACNET_APPLICATION_TAG_BIT_STRING,
                    BacnetBitString.Parse(fault ? "0100" : "0000")));

            Communicator.Storage.WriteProperty(oid, BacnetPropertyIds.PROP_RELIABILITY,
                new BacnetValue(BacnetApplicationTags.BACNET_APPLICATION_TAG_ENUMERATED,
                    fault ? BACNET_RELIABILITY_COMMUNICATION_FAILURE : BACNET_RELIABILITY_NO_FAULT_DETECTED));
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

        /// <summary>
        /// BACnet オブジェクト名のベース文字列。デバイス内で Object_Name の一意性が要求されるため、
        /// adr 方式は I²C アドレス由来 (A008..A119)、レガシー方式は InstanceBase 由来 (L12000..) の
        /// プレフィックスを必ず付ける (子機ラベルの重複対策)。
        /// </summary>
        private static string DisplayName(ChildNode child)
        {
            string n = child.Name?.Trim() ?? "";
            string label = string.IsNullOrEmpty(n) ? $"Node_{child.DeviceId}" : n;
            return child.I2cAddr == 0 ? $"L{child.InstanceBase}_{label}" : $"A{child.I2cAddr:D3}_{label}";
        }

        /// <summary>レガシー対応表 (DeviceID → InstanceBase) を読み込み、次の発番位置を復元する。</summary>
        private void LoadLegacyMap()
        {
            try
            {
                if (!File.Exists(legacyMapPath)) return;

                var map = System.Text.Json.JsonSerializer.Deserialize<Dictionary<uint, uint>>(File.ReadAllText(legacyMapPath));
                if (map == null) return;

                foreach (var kv in map)
                {
                    legacyBases[kv.Key] = kv.Value;
                    if (kv.Value >= nextLegacyBase) nextLegacyBase = kv.Value + INSTANCE_BLOCK;
                }
                Console.WriteLine($"[INFO] loaded {legacyBases.Count} legacy instance mapping(s) from {LEGACY_MAP_FILE}.");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[WARN] failed to load {LEGACY_MAP_FILE}: {ex.Message}");
            }
        }

        /// <summary>レガシー対応表をファイルへ保存する (発番・削除のたびに全書き)。</summary>
        private void SaveLegacyMap()
        {
            try
            {
                var opts = new System.Text.Json.JsonSerializerOptions { WriteIndented = true };
                File.WriteAllText(legacyMapPath, System.Text.Json.JsonSerializer.Serialize(legacyBases, opts));
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[WARN] failed to save {LEGACY_MAP_FILE}: {ex.Message}");
            }
        }

        #endregion

        /// <summary>子機 1 つの登録情報 (PoEMDevice 内部のみで使用)</summary>
        private sealed class ChildNode
        {
            public byte I2cAddr;
            public uint DeviceId;
            public uint InstanceBase;
            public string Name = "";
            public int DataCount;
            public ushort[] UnitTypes = System.Array.Empty<ushort>();

            /// <summary>直近に BACnet へ反映した status1 (FAULT/RELIABILITY の差分更新用)</summary>
            public byte LastStatus1;
        }
    }
}
