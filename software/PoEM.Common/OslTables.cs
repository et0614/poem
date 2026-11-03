namespace PoEM.Common
{
  /// <summary>
  /// 表示に使う名前の表 (OSL 共通レジスタ v2 アドレス表 §6)。
  /// PoEMServer の BACnet オブジェクトと PoEM Link アプリの画面の両方が使う。
  /// </summary>
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

    /// <summary>PoEM の子機の機種 (model_id → 表示名)。機種を足すときはここに加える</summary>
    private static readonly Dictionary<ushort, string> ModelNames = new()
    {
      [0x0101] = "温湿度センサ (SHT4x)",
      [0x0102] = "多点温度センサ (TMP117×3)",
      [0x0201] = "風速センサ (熱線式)",
      [0x0301] = "絶対圧センサ (BMP585)",
      [0x0302] = "差圧センサ (SDP810-125Pa)",
      [0x0303] = "差圧センサ (SDP810-500Pa)",
    };

    public static bool TryGetModelName(ushort model, out string name) => ModelNames.TryGetValue(model, out name!);

    /// <summary>機種の表示名。表に無ければ "機種 0xNNNN"</summary>
    public static string ModelName(ushort model) => TryGetModelName(model, out var n) ? n : $"機種 0x{model:X4}";
  }
}
