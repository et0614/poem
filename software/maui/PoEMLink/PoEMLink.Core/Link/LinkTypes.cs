namespace PoEMLink.Link
{
  /// <summary>コマンド番号 (通信仕様 §4.1)。応答は同じ番号に 0x40 を足した値</summary>
  public static class LinkCommand
  {
    public const byte GetInfo = 0x01;
    public const byte SetConfig = 0x02;
    public const byte Probe = 0x10;
    public const byte Scan = 0x11;
    public const byte Read = 0x20;
    public const byte Write = 0x21;
    public const byte WriteRead = 0x22;
    public const byte Batch = 0x30;
    public const byte BusStatus = 0x31;
    public const byte BusRecover = 0x32;
    public const byte Led = 0x38;
    public const byte Reboot = 0x3E;

    public const byte ResponseBit = 0x40;
  }

  /// <summary>結果コード (通信仕様 §5)</summary>
  public enum LinkResult : byte
  {
    Ok = 0x00,
    NackAddr = 0x01,
    NackData = 0x02,
    Timeout = 0x03,
    BusBusy = 0x04,
    ArbLost = 0x05,
    BusStuck = 0x06,
    BusError = 0x07,
    BadLength = 0x10,
    BadParam = 0x11,
    UnknownCmd = 0x12,
    BadEncoding = 0x13,
  }

  /// <summary>GET_INFO の機能ビット</summary>
  [Flags]
  public enum LinkFeatures : ushort
  {
    None = 0,
    Batch = 0x0001,
    LedIdentify = 0x0002,
    BusRecover = 0x0004,
  }

  /// <summary>GET_INFO の応答 (通信仕様 §4.2)</summary>
  public sealed record LinkInfo(
    byte ProtocolVersion,
    string Firmware,
    int MaxTransfer,
    int MaxBatch,
    int SpeedKHz,
    int TimeoutMs,
    LinkFeatures Features,
    byte[] Serial)
  {
    /// <summary>マイコン固有番号を 16 進 32 文字で</summary>
    public string SerialHex => Convert.ToHexString(Serial);
  }

  /// <summary>1 回の I²C 転送の結果</summary>
  /// <param name="Length">読めたバイト数 (WRITE なら ACK されたバイト数)</param>
  /// <param name="Data">読んだデータ (WRITE なら空)</param>
  public readonly record struct I2cResult(LinkResult Result, int Length, byte[] Data)
  {
    public bool IsOk => Result == LinkResult.Ok;
  }

  /// <summary>BATCH の 1 操作 (通信仕様 §4.9)</summary>
  public sealed record BatchOp(byte Command, byte Address, byte[] WriteData, int ReadLength)
  {
    public static BatchOp Read(byte addr, int n) => new(LinkCommand.Read, addr, Array.Empty<byte>(), n);
    public static BatchOp Write(byte addr, byte[] data) => new(LinkCommand.Write, addr, data, 0);
    public static BatchOp WriteRead(byte addr, byte[] w, int n) => new(LinkCommand.WriteRead, addr, w, n);

    /// <summary>要求の中でのバイト数</summary>
    internal int RequestSize => Command switch
    {
      LinkCommand.Read => 4,
      LinkCommand.Write => 4 + WriteData.Length,
      _ => 6 + WriteData.Length,
    };

    /// <summary>応答の中でのバイト数 (結果 1 + 長さ 2 + データ)</summary>
    internal int ResponseSize => 3 + (Command == LinkCommand.Write ? 0 : ReadLength);
  }

  /// <summary>BUS_STATUS の応答 (通信仕様 §4.10)</summary>
  public sealed record LinkBusStatus(
    bool Sda, bool Scl, byte BusState,
    ushort Timeouts, ushort Nacks, ushort BusBusy, ushort ArbLost, ushort Recoveries)
  {
    public string BusStateName => BusState switch
    {
      1 => "空き",
      2 => "自分が保持",
      3 => "他が使用中",
      _ => "不明",
    };
  }

  /// <summary>Link が要求を受け付けなかった (形式の誤りなど)</summary>
  public class LinkException : Exception
  {
    public LinkResult? Result { get; }

    public LinkException(string message, LinkResult? result = null) : base(message) => Result = result;
  }

  /// <summary>時間内に応答が無かった</summary>
  public class LinkTimeoutException : LinkException
  {
    public LinkTimeoutException(string message) : base(message) { }
  }
}
