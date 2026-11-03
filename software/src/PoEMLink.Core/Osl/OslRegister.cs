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
}
