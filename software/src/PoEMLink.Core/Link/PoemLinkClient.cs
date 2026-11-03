using System.Buffers.Binary;
using PoEMLink.Midi;

namespace PoEMLink.Link
{
  /// <summary>
  /// PoEM Link のブリッジ層 (通信仕様の L3)。要求を SysEx にして送り、応答を待って解釈する。
  ///
  /// - 要求は 1 つずつ送る (応答を受けてから次を送る、§3.3)。複数の呼び出し元から
  ///   同時に呼ばれても、内部で順番に並べる。
  /// - 応答は SEQ とコマンド番号で照合する。時間内に応答が無ければ、同じ SEQ で
  ///   <see cref="Retries"/> 回まで送り直す。
  /// - I²C の転送の失敗 (NACK など) は例外にせず、<see cref="I2cResult"/> で返す。
  ///   要求の形式の誤り (BAD_LENGTH など) と時間切れは <see cref="LinkException"/> にする。
  /// </summary>
  public sealed class PoemLinkClient : IDisposable
  {
    /// <summary>開発中のメーカー ID (非商用)</summary>
    public const byte DevelopmentManufacturerId = 0x7D;

    /// <summary>製品コード (PoEM Link)</summary>
    public const byte ProductCode = 0x02;

    private readonly ILinkTransport _transport;
    private readonly byte[] _manufacturerId;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly object _sync = new();

    private byte _seq;
    private TaskCompletionSource<byte[]>? _pending;
    private byte _pendingSeq;
    private byte _pendingCmd;

    /// <summary>1 要求あたりの待ち時間</summary>
    public TimeSpan Timeout { get; set; } = TimeSpan.FromMilliseconds(500);

    /// <summary>時間切れのときに送り直す回数</summary>
    public int Retries { get; set; } = 1;

    public ILinkTransport Transport => _transport;

    /// <param name="manufacturerId">メーカー ID。1 バイト、または 00 で始まる 3 バイト</param>
    public PoemLinkClient(ILinkTransport transport, byte[]? manufacturerId = null)
    {
      _transport = transport;
      _manufacturerId = manufacturerId ?? new[] { DevelopmentManufacturerId };
      _transport.SysExReceived += OnSysExReceived;
    }

    public void Dispose()
    {
      _transport.SysExReceived -= OnSysExReceived;
      lock (_sync) _pending?.TrySetCanceled();
    }

    #region 要求と応答

    /// <summary>要求を送り、応答のペイロード (復号済み、先頭は結果コード) を返す</summary>
    public async Task<byte[]> RequestAsync(byte cmd, ReadOnlyMemory<byte> payload, CancellationToken ct = default)
    {
      await _gate.WaitAsync(ct).ConfigureAwait(false);
      try
      {
        if (!_transport.IsConnected)
          throw new LinkException("PoEM Link が接続されていません");

        byte seq = _seq = (byte)((_seq + 1) & 0x7F);
        var body = BuildBody(seq, cmd, payload.Span);

        for (int attempt = 0; attempt <= Retries; attempt++)
        {
          var tcs = new TaskCompletionSource<byte[]>(TaskCreationOptions.RunContinuationsAsynchronously);
          lock (_sync)
          {
            _pending = tcs;
            _pendingSeq = seq;
            _pendingCmd = cmd;
          }
          _transport.SendSysEx(body);

          var done = await Task.WhenAny(tcs.Task, Task.Delay(Timeout, ct)).ConfigureAwait(false);
          ct.ThrowIfCancellationRequested();
          if (done == tcs.Task)
          {
            var resp = await tcs.Task.ConfigureAwait(false);
            if (resp.Length == 0)
              throw new LinkException($"コマンド 0x{cmd:X2} の応答が空です");
            return resp;
          }
        }
        throw new LinkTimeoutException($"コマンド 0x{cmd:X2} の応答がありません");
      }
      finally
      {
        lock (_sync) _pending = null;
        _gate.Release();
      }
    }

    private byte[] BuildBody(byte seq, byte cmd, ReadOnlySpan<byte> payload)
    {
      var enc = SevenBit.Encode(payload);
      var body = new byte[_manufacturerId.Length + 3 + enc.Length];
      _manufacturerId.CopyTo(body, 0);
      int o = _manufacturerId.Length;
      body[o++] = ProductCode;
      body[o++] = seq;
      body[o++] = cmd;
      enc.CopyTo(body, o);
      return body;
    }

    private void OnSysExReceived(byte[] body)
    {
      // MFR (1 または 3 バイト)、PRODUCT、SEQ、CMD|0x40、符号化したペイロード
      int m = _manufacturerId.Length;
      if (body.Length < m + 3) return;
      for (int i = 0; i < m; i++)
        if (body[i] != _manufacturerId[i]) return;
      if (body[m] != ProductCode) return;
      byte seq = body[m + 1];
      byte cmd = body[m + 2];

      lock (_sync)
      {
        if (_pending == null || seq != _pendingSeq || cmd != (_pendingCmd | LinkCommand.ResponseBit)) return;
        var payload = SevenBit.Decode(body.AsSpan(m + 3));
        if (payload == null)
          _pending.TrySetException(new LinkException("応答の 7bit 符号化が不正です", LinkResult.BadEncoding));
        else
          _pending.TrySetResult(payload);
      }
    }

    /// <summary>I²C 以外の失敗 (形式の誤り) なら例外にする</summary>
    private static void ThrowIfRequestError(byte cmd, byte[] resp)
    {
      var r = (LinkResult)resp[0];
      if (r >= LinkResult.BadLength)
        throw new LinkException($"コマンド 0x{cmd:X2} が拒否されました ({r})", r);
    }

    private static I2cResult ParseTransfer(byte cmd, byte[] resp, bool hasData)
    {
      ThrowIfRequestError(cmd, resp);
      var r = (LinkResult)resp[0];
      if (resp.Length < 3) return new I2cResult(r, 0, Array.Empty<byte>());
      int len = BinaryPrimitives.ReadUInt16LittleEndian(resp.AsSpan(1));
      var data = hasData ? resp.AsSpan(3, Math.Min(len, resp.Length - 3)).ToArray() : Array.Empty<byte>();
      return new I2cResult(r, len, data);
    }

    #endregion

    #region コマンド

    public async Task<LinkInfo> GetInfoAsync(CancellationToken ct = default)
    {
      var r = await RequestAsync(LinkCommand.GetInfo, ReadOnlyMemory<byte>.Empty, ct).ConfigureAwait(false);
      ThrowIfRequestError(LinkCommand.GetInfo, r);
      if (r.Length < 31) throw new LinkException("GET_INFO の応答が短すぎます");
      var s = r.AsSpan();
      return new LinkInfo(
        ProtocolVersion: r[1],
        Firmware: $"{r[2]}.{r[3]}.{r[4]}",
        MaxTransfer: BinaryPrimitives.ReadUInt16LittleEndian(s[5..]),
        MaxBatch: BinaryPrimitives.ReadUInt16LittleEndian(s[7..]),
        SpeedKHz: BinaryPrimitives.ReadUInt16LittleEndian(s[9..]),
        TimeoutMs: BinaryPrimitives.ReadUInt16LittleEndian(s[11..]),
        Features: (LinkFeatures)BinaryPrimitives.ReadUInt16LittleEndian(s[13..]),
        Serial: s.Slice(15, 16).ToArray());
    }

    /// <summary>I²C の速度とタイムアウトを変える (0 なら変えない)。設定後の値を返す</summary>
    public async Task<(int SpeedKHz, int TimeoutMs)> SetConfigAsync(int speedKHz, int timeoutMs, CancellationToken ct = default)
    {
      var p = new byte[4];
      BinaryPrimitives.WriteUInt16LittleEndian(p, (ushort)speedKHz);
      BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(2), (ushort)timeoutMs);
      var r = await RequestAsync(LinkCommand.SetConfig, p, ct).ConfigureAwait(false);
      ThrowIfRequestError(LinkCommand.SetConfig, r);
      if (r.Length < 5) throw new LinkException("SET_CONFIG の応答が短すぎます");
      return (BinaryPrimitives.ReadUInt16LittleEndian(r.AsSpan(1)), BinaryPrimitives.ReadUInt16LittleEndian(r.AsSpan(3)));
    }

    /// <summary>機器がいるかを調べる (書き込みはしない)</summary>
    public async Task<LinkResult> ProbeAsync(byte addr, CancellationToken ct = default)
    {
      var r = await RequestAsync(LinkCommand.Probe, new[] { addr }, ct).ConfigureAwait(false);
      ThrowIfRequestError(LinkCommand.Probe, r);
      return (LinkResult)r[0];
    }

    /// <summary>アドレスの範囲を調べ、応答したアドレスを返す</summary>
    public async Task<IReadOnlyList<byte>> ScanAsync(byte first = 0x08, byte last = 0x77, CancellationToken ct = default)
    {
      var r = await RequestAsync(LinkCommand.Scan, new[] { first, last }, ct).ConfigureAwait(false);
      ThrowIfRequestError(LinkCommand.Scan, r);
      var found = new List<byte>();
      if ((LinkResult)r[0] != LinkResult.Ok || r.Length < 17) return found;
      for (int a = 0; a < 128; a++)
        if ((r[1 + a / 8] & (1 << (a % 8))) != 0) found.Add((byte)a);
      return found;
    }

    /// <summary>番地を送らずに読む</summary>
    public async Task<I2cResult> ReadAsync(byte addr, int length, CancellationToken ct = default)
    {
      var p = new byte[3];
      p[0] = addr;
      BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(1), (ushort)length);
      var r = await RequestAsync(LinkCommand.Read, p, ct).ConfigureAwait(false);
      return ParseTransfer(LinkCommand.Read, r, hasData: true);
    }

    public async Task<I2cResult> WriteAsync(byte addr, ReadOnlyMemory<byte> data, CancellationToken ct = default)
    {
      var p = new byte[1 + data.Length];
      p[0] = addr;
      data.Span.CopyTo(p.AsSpan(1));
      var r = await RequestAsync(LinkCommand.Write, p, ct).ConfigureAwait(false);
      return ParseTransfer(LinkCommand.Write, r, hasData: false);
    }

    /// <summary>書いてから Repeated START で読む (番地を指定した読み出し)</summary>
    public async Task<I2cResult> WriteReadAsync(byte addr, ReadOnlyMemory<byte> write, int readLength, CancellationToken ct = default)
    {
      var p = new byte[5 + write.Length];
      p[0] = addr;
      BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(1), (ushort)write.Length);
      write.Span.CopyTo(p.AsSpan(3));
      BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(3 + write.Length), (ushort)readLength);
      var r = await RequestAsync(LinkCommand.WriteRead, p, ct).ConfigureAwait(false);
      return ParseTransfer(LinkCommand.WriteRead, r, hasData: true);
    }

    /// <summary>
    /// 複数の転送を 1 往復で実行する。応答の大きさが MAX_BATCH を超えないよう、呼び出し元が分割する
    /// (<see cref="Osl.LinkI2cBus"/> が行う)。戻り値は実行した操作の結果 (中止したら操作の数より少ない)。
    /// </summary>
    public async Task<IReadOnlyList<I2cResult>> BatchAsync(IReadOnlyList<BatchOp> ops, bool stopOnError = false, CancellationToken ct = default)
    {
      if (ops.Count is < 1 or > 64) throw new ArgumentOutOfRangeException(nameof(ops), "操作の数は 1〜64");

      var p = new List<byte>(2 + ops.Sum(o => o.RequestSize)) { (byte)(stopOnError ? 1 : 0), (byte)ops.Count };
      foreach (var op in ops)
      {
        p.Add(op.Command);
        p.Add(op.Address);
        if (op.Command != LinkCommand.Read)
        {
          p.Add((byte)op.WriteData.Length);
          p.Add((byte)(op.WriteData.Length >> 8));
          p.AddRange(op.WriteData);
        }
        if (op.Command != LinkCommand.Write)
        {
          p.Add((byte)op.ReadLength);
          p.Add((byte)(op.ReadLength >> 8));
        }
      }

      var r = await RequestAsync(LinkCommand.Batch, p.ToArray(), ct).ConfigureAwait(false);
      ThrowIfRequestError(LinkCommand.Batch, r);
      if (r.Length < 2) throw new LinkException("BATCH の応答が短すぎます");

      int executed = r[1];
      var results = new List<I2cResult>(executed);
      int o = 2;
      for (int i = 0; i < executed && i < ops.Count; i++)
      {
        if (o + 3 > r.Length) throw new LinkException("BATCH の応答が途中で切れています");
        var res = (LinkResult)r[o];
        int len = BinaryPrimitives.ReadUInt16LittleEndian(r.AsSpan(o + 1));
        o += 3;
        byte[] data = Array.Empty<byte>();
        if (ops[i].Command != LinkCommand.Write)
        {
          if (o + len > r.Length) throw new LinkException("BATCH の応答が途中で切れています");
          data = r.AsSpan(o, len).ToArray();
          o += len;
        }
        results.Add(new I2cResult(res, len, data));
      }
      return results;
    }

    public async Task<LinkBusStatus> GetBusStatusAsync(CancellationToken ct = default)
    {
      var r = await RequestAsync(LinkCommand.BusStatus, ReadOnlyMemory<byte>.Empty, ct).ConfigureAwait(false);
      ThrowIfRequestError(LinkCommand.BusStatus, r);
      if (r.Length < 14) throw new LinkException("BUS_STATUS の応答が短すぎます");
      var s = r.AsSpan();
      return new LinkBusStatus(
        Sda: r[1] != 0, Scl: r[2] != 0, BusState: r[3],
        Timeouts: BinaryPrimitives.ReadUInt16LittleEndian(s[4..]),
        Nacks: BinaryPrimitives.ReadUInt16LittleEndian(s[6..]),
        BusBusy: BinaryPrimitives.ReadUInt16LittleEndian(s[8..]),
        ArbLost: BinaryPrimitives.ReadUInt16LittleEndian(s[10..]),
        Recoveries: BinaryPrimitives.ReadUInt16LittleEndian(s[12..]));
    }

    /// <summary>バスの固着からの復帰を試みる。OK か BUS_STUCK を返す</summary>
    public async Task<LinkResult> BusRecoverAsync(CancellationToken ct = default)
    {
      var r = await RequestAsync(LinkCommand.BusRecover, ReadOnlyMemory<byte>.Empty, ct).ConfigureAwait(false);
      ThrowIfRequestError(LinkCommand.BusRecover, r);
      return (LinkResult)r[0];
    }

    /// <summary>LED を識別の点滅にする (seconds = 0 で自動の表示に戻す)</summary>
    public async Task IdentifyAsync(int seconds, CancellationToken ct = default)
    {
      byte mode = (byte)(seconds > 0 ? 1 : 0);
      var r = await RequestAsync(LinkCommand.Led, new[] { mode, (byte)Math.Clamp(seconds, 0, 255) }, ct).ConfigureAwait(false);
      ThrowIfRequestError(LinkCommand.Led, r);
    }

    public async Task RebootAsync(CancellationToken ct = default)
    {
      var r = await RequestAsync(LinkCommand.Reboot, ReadOnlyMemory<byte>.Empty, ct).ConfigureAwait(false);
      ThrowIfRequestError(LinkCommand.Reboot, r);
    }

    #endregion
  }
}
