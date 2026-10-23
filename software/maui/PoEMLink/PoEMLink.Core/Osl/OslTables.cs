namespace PoEMLink.Osl
{
  /// <summary>番地と定数 (OSL 共通レジスタ v2 アドレス表 §3, §4)</summary>
  public static class OslRegister
  {
    public const ushort Ident = 0x0000;      // 識別 32 B (CRC 込み)
    public const ushort AddrKey = 0x0020;    // アドレス変更 (鍵 + 新アドレス)
    public const ushort Name = 0x0040;       // 名前 64 B + CRC
    public const ushort ChDesc = 0x0100;     // チャネル記述 16 B × N + CRC
    public const ushort Poll = 0x0300;       // POLL (番地を設定した時点の値を固定して返す)
    public const ushort ExtIdent = 0x0400;   // 拡張識別 8 B + CRC
    public const ushort ExtAttr = 0x0420;    // 拡張チャネル属性 4 B × N + CRC
    public const ushort ExtStatus = 0x0480;  // 拡張ステータス 10 B + CRC
    public const ushort CmdKey = 0x04C0;     // コマンド (鍵 + コマンド + 引数)

    public const int IdentLength = 32;
    public const int NameLength = 64;
    public const byte Key = 0xA5;

    public const byte CmdReset = 0x01;
    public const byte CmdIdentify = 0x02;

    public const byte MinAddress = 0x08;
    public const byte MaxAddress = 0x77;
  }

  [Flags]
  public enum OslCapability : ushort
  {
    None = 0,
    ChannelAttributes = 0x0001,
    ExtStatus = 0x0002,
    CmdReset = 0x0004,
    CmdIdentify = 0x0008,
  }

  public enum OslValueType : byte
  {
    Float32 = 0,
    Int32 = 1,
    UInt32 = 2,
  }

  [Flags]
  public enum OslChannelFlags : byte
  {
    None = 0,
    Counter = 0x01,   // 積算値 (一巡あり)
    Auxiliary = 0x02, // 補助値 (センサ内部温度・生電圧など)
  }

  /// <summary>表示に使う名前の表</summary>
  public static class OslTables
  {
    /// <summary>BACnet Engineering Units のうち、表示に使うもの</summary>
    private static readonly Dictionary<ushort, string> UnitNames = new()
    {
      [5] = "V",
      [29] = "%RH",
      [37] = "lx",
      [53] = "Pa",
      [54] = "kPa",
      [62] = "°C",
      [63] = "K",
      [74] = "m/s",
      [95] = "",
      [96] = "ppm",
      [98] = "%",
    };

    public static string UnitName(ushort unit) => UnitNames.TryGetValue(unit, out var s) ? s : $"unit{unit}";

    /// <summary>チャネルの状態 (BACnet Reliability の値)</summary>
    public static string StatusName(byte status) => status switch
    {
      0 => "正常",
      1 => "センサ応答なし",
      2 => "上限超過",
      3 => "下限未満",
      7 => "異常",
      12 => "通信失敗",
      64 => "準備中",
      _ => $"状態 {status}",
    };

    public static string ResetCauseName(byte cause) => cause switch
    {
      0 => "不明",
      1 => "電源投入",
      2 => "電圧低下",
      3 => "ウォッチドッグ",
      4 => "ソフトウェア",
      5 => "外部リセット",
      6 => "その他",
      _ => cause.ToString(),
    };

    public static string VendorName(ushort vendor) => vendor == 0x0001 ? "PoEM" : $"0x{vendor:X4}";

    public static string ModelName(ushort model) => model switch
    {
      0x0101 => "温湿度センサ (SHT4x)",
      0x0102 => "多点温度センサ (TMP117×3)",
      0x0201 => "風速センサ (熱線式)",
      0x0301 => "絶対圧センサ (BMP585)",
      0x0302 => "差圧センサ (SDP810-125Pa)",
      0x0303 => "差圧センサ (SDP810-500Pa)",
      _ => $"機種 0x{model:X4}",
    };
  }
}
