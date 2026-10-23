namespace PoEMLink.Osl
{
  /// <summary>走査の結果</summary>
  /// <param name="Devices">OSL 子機 (識別・名前・チャネルを読み終えたもの)</param>
  /// <param name="Unknown">応答したが OSL 子機と確認できなかったアドレス</param>
  public sealed record OslScanResult(IReadOnlyList<OslDevice> Devices, IReadOnlyList<byte> Unknown);

  public static class OslScanner
  {
    /// <summary>
    /// OSL 子機を探す (アドレス表 §7.1)。応答したアドレスから番地を送らずに 32 B 読み、
    /// "OSL" と CRC を確かめる。この段階では書き込みを一切しないので、同じバスにつながった
    /// 他の I²C 機器を書き換えない。OSL 子機と確認できた相手だけ、番地を指定して読み進める。
    /// </summary>
    public static async Task<OslScanResult> ScanAsync(II2cBus bus, byte first = OslRegister.MinAddress,
      byte last = OslRegister.MaxAddress, CancellationToken ct = default)
    {
      var devices = new List<OslDevice>();
      var unknown = new List<byte>();
      foreach (var addr in await bus.ScanAsync(first, last, ct).ConfigureAwait(false))
      {
        OslDevice? dev = null;
        for (int i = 0; i < 2 && dev == null; i++)
        {
          var b = await bus.ReadAsync(addr, OslRegister.IdentLength, ct).ConfigureAwait(false);
          if (b != null && OslDevice.ParseIdentity(b) != null)
            dev = new OslDevice(bus, addr);
        }
        if (dev == null)
        {
          unknown.Add(addr);
          continue;
        }
        try
        {
          await dev.LoadAsync(ct).ConfigureAwait(false);
          devices.Add(dev);
        }
        catch (OslException)
        {
          unknown.Add(addr);
        }
      }
      return new OslScanResult(devices, unknown);
    }
  }
}
