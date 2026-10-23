namespace PoEMLink.Osl
{
  /// <summary>
  /// OSL 層 (L4) が使う I²C の通信手段。Python の osl.py の「バス」と同じ役割。
  /// PoEM Link では <see cref="LinkI2cBus"/> が実装する。
  /// 失敗 (NACK など) は例外にせず、false や null で返す。
  /// </summary>
  public interface II2cBus
  {
    /// <summary>1 回の転送の上限 [バイト]</summary>
    int MaxTransfer { get; }

    /// <summary>範囲内で応答したアドレス (書き込みはしない)</summary>
    Task<IReadOnlyList<byte>> ScanAsync(byte first, byte last, CancellationToken ct = default);

    /// <summary>番地を送らずに n バイト読む。失敗なら null</summary>
    Task<byte[]?> ReadAsync(byte addr, int n, CancellationToken ct = default);

    Task<bool> WriteAsync(byte addr, ReadOnlyMemory<byte> data, CancellationToken ct = default);

    /// <summary>書いてから Repeated START で n バイト読む。失敗なら null</summary>
    Task<byte[]?> WriteReadAsync(byte addr, ReadOnlyMemory<byte> write, int n, CancellationToken ct = default);

    /// <summary>
    /// 複数の WRITE_READ をまとめて行う (毎周期の POLL 用)。結果は要求と同じ順で、失敗は null。
    /// まとめて送る手段の無いバスは、1 つずつ行えばよい。
    /// </summary>
    Task<IReadOnlyList<byte[]?>> WriteReadManyAsync(IReadOnlyList<(byte Addr, byte[] Write, int ReadLength)> requests, CancellationToken ct = default);
  }
}
