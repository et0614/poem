using System.Buffers.Binary;
using PoEMLink.Link;
using PoEMLink.Midi;
using PoEMLink.Osl;

namespace PoEMLink.Simulation
{
  /// <summary>
  /// PoEM Link の模擬 (ファームウェア firmware/poem_link.X の bridge.c と同じ応答を返す)。
  /// 実機の代わりに <see cref="ILinkTransport"/> として差し込み、模擬の I²C 機器を読み書きする。
  /// 子機の抜き差しは <see cref="Attach"/> / <see cref="Detach"/> で再現する。
  /// </summary>
  public sealed class SimLinkTransport : ILinkTransport
  {
    private const int MaxXfer = 256;
    private const int MaxBatch = 512;

    private readonly List<ISimI2cDevice> _devices = new();
    private readonly object _sync = new();
    private readonly byte[] _serial = Convert.FromHexString("1E9A0C4D5F3B2A19876543210FEDCBA9");
    private int _speedKHz = 100;
    private int _timeoutMs = 100;
    private ushort _nacks;

    public event Action<byte[]>? SysExReceived;
    public event Action<bool>? ConnectionChanged;

    public bool IsConnected { get; private set; }

    public string? DeviceName => IsConnected ? "PoEM Link 1E9A0C4D (模擬)" : null;

    /// <summary>1 往復にかかる時間 (USB と I²C の転送の目安)</summary>
    public TimeSpan Latency { get; set; } = TimeSpan.FromMilliseconds(4);

    /// <summary>Link の識別点滅の期限 (表示確認用)</summary>
    public DateTime IdentifyUntil { get; private set; }

    public IReadOnlyList<ISimI2cDevice> Devices
    {
      get { lock (_sync) return _devices.ToList(); }
    }

    public void Connect()
    {
      IsConnected = true;
      ConnectionChanged?.Invoke(true);
    }

    public void Disconnect()
    {
      IsConnected = false;
      ConnectionChanged?.Invoke(false);
    }

    public void Attach(ISimI2cDevice dev)
    {
      lock (_sync) _devices.Add(dev);
    }

    public void Detach(ISimI2cDevice dev)
    {
      lock (_sync) _devices.Remove(dev);
    }

    public void SendSysEx(byte[] body)
    {
      if (!IsConnected) return;
      _ = Task.Run(async () =>
      {
        await Task.Delay(Latency).ConfigureAwait(false);
        var resp = Handle(body);
        if (resp != null && IsConnected) SysExReceived?.Invoke(resp);
      });
    }

    #region ファームウェアの再現

    private byte[]? Handle(byte[] body)
    {
      // MFR 7D、PRODUCT 02、SEQ、CMD、符号化したペイロード。一致しなければ黙って捨てる
      if (body.Length < 4 || body[0] != PoemLinkClient.DevelopmentManufacturerId || body[1] != PoemLinkClient.ProductCode)
        return null;
      byte seq = body[2], cmd = body[3];
      var p = SevenBit.Decode(body.AsSpan(4));

      var resp = new List<byte>();
      if (p == null) resp.Add((byte)LinkResult.BadEncoding);
      else lock (_sync) Execute(cmd, p, resp);

      var enc = SevenBit.Encode(resp.ToArray());
      var r = new byte[4 + enc.Length];
      r[0] = PoemLinkClient.DevelopmentManufacturerId;
      r[1] = PoemLinkClient.ProductCode;
      r[2] = seq;
      r[3] = (byte)(cmd | LinkCommand.ResponseBit);
      enc.CopyTo(r, 4);
      return r;
    }

    private static void AddU16(List<byte> l, int v)
    {
      l.Add((byte)v);
      l.Add((byte)(v >> 8));
    }

    private static int U16(byte[] p, int o) => BinaryPrimitives.ReadUInt16LittleEndian(p.AsSpan(o));

    private void Execute(byte cmd, byte[] p, List<byte> r)
    {
      switch (cmd)
      {
        case LinkCommand.GetInfo:
          r.Add(0); r.Add(1);
          r.AddRange(new byte[] { 0, 1, 0 });
          AddU16(r, MaxXfer); AddU16(r, MaxBatch); AddU16(r, _speedKHz); AddU16(r, _timeoutMs);
          AddU16(r, (int)(LinkFeatures.Batch | LinkFeatures.LedIdentify | LinkFeatures.BusRecover));
          r.AddRange(_serial);
          break;

        case LinkCommand.SetConfig:
          {
            if (p.Length != 4) { r.Add((byte)LinkResult.BadLength); break; }
            int s = U16(p, 0), t = U16(p, 2);
            if ((s != 0 && s != 100 && s != 400) || (t != 0 && (t < 10 || t > 1000))) { r.Add((byte)LinkResult.BadParam); break; }
            if (s != 0) _speedKHz = s;
            if (t != 0) _timeoutMs = t;
            r.Add(0); AddU16(r, _speedKHz); AddU16(r, _timeoutMs);
            break;
          }

        case LinkCommand.Probe:
          if (p.Length != 1) { r.Add((byte)LinkResult.BadLength); break; }
          r.Add((byte)(DoRead(p[0], 1).Result));
          break;

        case LinkCommand.Scan:
          {
            if (p.Length != 2) { r.Add((byte)LinkResult.BadLength); break; }
            var bits = new byte[16];
            for (int a = p[0]; a <= p[1] && a < 128; a++)
              if (DoRead((byte)a, 1).IsOk) bits[a / 8] |= (byte)(1 << (a % 8));
            r.Add(0); r.AddRange(bits);
            break;
          }

        case LinkCommand.Read:
          {
            if (p.Length != 3) { r.Add((byte)LinkResult.BadLength); break; }
            int n = U16(p, 1);
            if (n is < 1 or > MaxXfer) { r.Add((byte)LinkResult.BadLength); break; }
            AddTransfer(r, DoRead(p[0], n), withData: true);
            break;
          }

        case LinkCommand.Write:
          if (p.Length < 1 || p.Length - 1 > MaxXfer) { r.Add((byte)LinkResult.BadLength); break; }
          AddTransfer(r, DoWrite(p[0], p.AsSpan(1)), withData: false);
          break;

        case LinkCommand.WriteRead:
          {
            if (p.Length < 5) { r.Add((byte)LinkResult.BadLength); break; }
            int w = U16(p, 1);
            if (w is < 1 or > MaxXfer || p.Length != 5 + w) { r.Add((byte)LinkResult.BadLength); break; }
            int n = U16(p, 3 + w);
            if (n is < 1 or > MaxXfer) { r.Add((byte)LinkResult.BadLength); break; }
            AddTransfer(r, DoWriteRead(p[0], p.AsSpan(3, w), n), withData: true);
            break;
          }

        case LinkCommand.Batch:
          ExecuteBatch(p, r);
          break;

        case LinkCommand.BusStatus:
          r.Add(0); r.Add(1); r.Add(1); r.Add(1);
          AddU16(r, 0); AddU16(r, _nacks); AddU16(r, 0); AddU16(r, 0); AddU16(r, 0);
          break;

        case LinkCommand.BusRecover:
          r.Add(0); r.Add(1); r.Add(1);
          break;

        case LinkCommand.Led:
          if (p.Length != 2 || p[0] > 1) { r.Add((byte)LinkResult.BadParam); break; }
          IdentifyUntil = p[0] == 1 ? DateTime.Now.AddSeconds(p[1]) : DateTime.MinValue;
          r.Add(0);
          break;

        case LinkCommand.Reboot:
          r.Add(0);
          break;

        default:
          r.Add((byte)LinkResult.UnknownCmd);
          break;
      }
    }

    private static void AddTransfer(List<byte> r, I2cResult x, bool withData)
    {
      r.Add((byte)x.Result);
      AddU16(r, x.Length);
      if (withData) r.AddRange(x.Data);
    }

    private void ExecuteBatch(byte[] p, List<byte> r)
    {
      if (p.Length < 2 || p[1] is < 1 or > 64) { r.Add((byte)LinkResult.BadLength); return; }
      bool stopOnError = (p[0] & 1) != 0;
      int k = p[1];

      // まず全体を検証し、応答の大きさを確かめる
      var ops = new List<(byte Cmd, byte Addr, int WOfs, int WLen, int RLen)>();
      int o = 2, respData = 0;
      for (int i = 0; i < k; i++)
      {
        if (o + 2 > p.Length) { r.Add((byte)LinkResult.BadLength); return; }
        byte c = p[o], a = p[o + 1];
        o += 2;
        int wofs = 0, wlen = 0, rlen = 0;
        if (c == LinkCommand.Write || c == LinkCommand.WriteRead)
        {
          if (o + 2 > p.Length) { r.Add((byte)LinkResult.BadLength); return; }
          wlen = U16(p, o); o += 2; wofs = o; o += wlen;
        }
        if (c == LinkCommand.Read || c == LinkCommand.WriteRead)
        {
          if (o + 2 > p.Length) { r.Add((byte)LinkResult.BadLength); return; }
          rlen = U16(p, o); o += 2;
          if (rlen is < 1 or > MaxXfer) { r.Add((byte)LinkResult.BadLength); return; }
        }
        if (c != LinkCommand.Read && c != LinkCommand.Write && c != LinkCommand.WriteRead) { r.Add((byte)LinkResult.BadParam); return; }
        if (o > p.Length) { r.Add((byte)LinkResult.BadLength); return; }
        respData += rlen;
        ops.Add((c, a, wofs, wlen, rlen));
      }
      if (o != p.Length || respData > MaxBatch) { r.Add((byte)LinkResult.BadLength); return; }

      var results = new List<byte>();
      LinkResult overall = LinkResult.Ok;
      int executed = 0;
      foreach (var op in ops)
      {
        var x = op.Cmd switch
        {
          LinkCommand.Read => DoRead(op.Addr, op.RLen),
          LinkCommand.Write => DoWrite(op.Addr, p.AsSpan(op.WOfs, op.WLen)),
          _ => DoWriteRead(op.Addr, p.AsSpan(op.WOfs, op.WLen), op.RLen),
        };
        AddTransfer(results, x, withData: op.Cmd != LinkCommand.Write);
        executed++;
        if (!x.IsOk && overall == LinkResult.Ok) overall = x.Result;
        if (!x.IsOk && stopOnError) break;
      }
      r.Add((byte)overall);
      r.Add((byte)executed);
      r.AddRange(results);
    }

    private ISimI2cDevice? Find(byte addr) => _devices.FirstOrDefault(d => d.Address == addr);

    private I2cResult DoRead(byte addr, int n)
    {
      var d = Find(addr);
      if (d == null) { _nacks++; return new I2cResult(LinkResult.NackAddr, 0, Array.Empty<byte>()); }
      d.Start();
      var data = d.Read(n);
      d.Stop();
      return new I2cResult(LinkResult.Ok, data.Length, data);
    }

    private I2cResult DoWrite(byte addr, ReadOnlySpan<byte> w)
    {
      var d = Find(addr);
      if (d == null) { _nacks++; return new I2cResult(LinkResult.NackAddr, 0, Array.Empty<byte>()); }
      d.Start();
      int acked = d.Write(w);
      d.Stop();
      return new I2cResult(acked == w.Length ? LinkResult.Ok : LinkResult.NackData, acked, Array.Empty<byte>());
    }

    private I2cResult DoWriteRead(byte addr, ReadOnlySpan<byte> w, int n)
    {
      var d = Find(addr);
      if (d == null) { _nacks++; return new I2cResult(LinkResult.NackAddr, 0, Array.Empty<byte>()); }
      d.Start();
      d.Write(w);
      var data = d.Read(n);
      d.Stop();
      return new I2cResult(LinkResult.Ok, data.Length, data);
    }

    #endregion
  }
}
