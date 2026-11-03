using System.Buffers.Binary;
using System.Text;
using PoEM.Common;

namespace PoEMLink.Osl
{
  /// <summary>識別区画 (0x0000)</summary>
  public sealed record OslIdentity(
    byte Spec, ushort Vendor, ushort Model, string Firmware,
    int ChannelCount, ushort PeriodMs, byte[] DeviceId)
  {
    /// <summary>個体 ID (16 B) を 16 進 32 文字で</summary>
    public string Uid => Convert.ToHexString(DeviceId);

    public string ModelName => OslTables.ModelName(Model);

    public string VendorName => OslTables.VendorName(Vendor);
  }

  /// <summary>チャネル記述 (0x0100) と拡張チャネル属性 (0x0420)</summary>
  public sealed record OslChannel(
    int Index, string Name, ushort Unit, OslValueType Type, byte Precision, OslChannelFlags Flags)
  {
    public string UnitName => OslTables.UnitName(Unit);

    public bool IsAuxiliary => Flags.HasFlag(OslChannelFlags.Auxiliary);

    /// <summary>
    /// 値を表示用の文字列にする (有効桁数 Precision、0 なら 6 桁)。
    /// 末尾の 0 も残す (25.00 を 25 にしない) ので、値が変わっても桁の並びがずれない。
    /// </summary>
    public string Format(double value)
    {
      if (double.IsNaN(value) || double.IsInfinity(value)) return "—";
      if (Type != OslValueType.Float32) return ((long)value).ToString();
      int p = Precision == 0 ? 6 : Precision;
      int digits = value == 0 ? 1 : (int)Math.Floor(Math.Log10(Math.Abs(value))) + 1;
      int decimals = Math.Clamp(p - digits, 0, 10);
      return value.ToString("F" + decimals, System.Globalization.CultureInfo.InvariantCulture);
    }
  }

  /// <summary>拡張識別 (0x0400)</summary>
  public sealed record OslExtIdentity(OslCapability Capability, byte HwRev, ushort AvgWindowMs, ushort WarmupMs);

  /// <summary>拡張ステータス (0x0480)</summary>
  public sealed record OslExtStatus(uint UptimeSec, byte NodeFlags, byte ResetCause, ushort ErrI2c, ushort ErrSensor)
  {
    public string ResetCauseName => OslTables.ResetCauseName(ResetCause);
  }

  /// <summary>POLL で読んだ 1 回分の計測値</summary>
  public sealed record OslSample(ushort Seq, double[] Values, byte[] Status);

  public class OslException : Exception
  {
    public OslException(string message) : base(message) { }
  }

  /// <summary>
  /// 1 台の OSL v2 子機 (Python の osl.OslDevice の移植。ユーザー領域は扱わない)。
  /// <see cref="LoadAsync"/> で識別・名前・チャネルを読んでから使う。
  /// </summary>
  public sealed class OslDevice
  {
    private readonly II2cBus _bus;

    public byte Address { get; private set; }

    public OslIdentity Identity { get; private set; } = null!;

    public string Name { get; private set; } = "";

    public IReadOnlyList<OslChannel> Channels { get; private set; } = Array.Empty<OslChannel>();

    /// <summary>拡張識別。未実装なら null</summary>
    public OslExtIdentity? ExtIdentity { get; private set; }

    /// <summary>POLL 区画の長さ: seq 2 + 値 4N + 状態 N + 詰め物 (N が奇数のとき 1) + CRC 2</summary>
    public int PollLength => 2 + 5 * Channels.Count + (Channels.Count & 1) + 2;

    public bool HasExtStatus => ExtIdentity?.Capability.HasFlag(OslCapability.ExtStatus) == true;

    public bool CanIdentify => ExtIdentity?.Capability.HasFlag(OslCapability.CmdIdentify) == true;

    public OslDevice(II2cBus bus, byte address)
    {
      _bus = bus;
      Address = address;
    }

    #region 読み書きの下請け

    private static byte[] RegBytes(ushort reg) => new[] { (byte)(reg >> 8), (byte)reg };

    /// <summary>番地 reg から n バイト読む。1 回の転送の上限を超えるときは分けて読む</summary>
    public async Task<byte[]?> ReadRegAsync(ushort reg, int n, CancellationToken ct = default)
    {
      if (n <= _bus.MaxTransfer)
        return await _bus.WriteReadAsync(Address, RegBytes(reg), n, ct).ConfigureAwait(false);

      var buf = new byte[n];
      for (int ofs = 0; ofs < n; ofs += _bus.MaxTransfer)
      {
        int len = Math.Min(_bus.MaxTransfer, n - ofs);
        var part = await _bus.WriteReadAsync(Address, RegBytes((ushort)(reg + ofs)), len, ct).ConfigureAwait(false);
        if (part == null) return null;
        part.CopyTo(buf, ofs);
      }
      return buf;
    }

    public Task<bool> WriteRegAsync(ushort reg, ReadOnlySpan<byte> data, CancellationToken ct = default)
    {
      var buf = new byte[2 + data.Length];
      buf[0] = (byte)(reg >> 8);
      buf[1] = (byte)reg;
      data.CopyTo(buf.AsSpan(2));
      return _bus.WriteAsync(Address, buf, ct);
    }

    /// <summary>CRC 込みの区画を読み、一致するまで読み直す</summary>
    private async Task<byte[]> ReadCheckedAsync(ushort reg, int n, CancellationToken ct, int tries = 3)
    {
      for (int i = 0; i < tries; i++)
      {
        var b = await ReadRegAsync(reg, n, ct).ConfigureAwait(false);
        if (b != null && OslCrc.Check(b)) return b;
      }
      throw new OslException($"0x{Address:X2} 0x{reg:X4}: CRC 不一致または読み出し失敗");
    }

    private static string CString(ReadOnlySpan<byte> raw)
    {
      int z = raw.IndexOf((byte)0);
      return Encoding.UTF8.GetString(z < 0 ? raw : raw[..z]);
    }

    #endregion

    #region 必須区画

    /// <summary>識別・名前・拡張識別・チャネルを読む</summary>
    public async Task LoadAsync(CancellationToken ct = default)
    {
      Identity = ParseIdentity(await ReadCheckedAsync(OslRegister.Ident, OslRegister.IdentLength, ct).ConfigureAwait(false))
        ?? throw new OslException($"0x{Address:X2}: OSL 子機ではない");
      Name = await ReadNameAsync(ct).ConfigureAwait(false);
      ExtIdentity = await ReadExtIdentityAsync(ct).ConfigureAwait(false);
      Channels = await ReadChannelsAsync(ct).ConfigureAwait(false);
    }

    /// <summary>識別区画を解釈する。"OSL" で始まり CRC が合わなければ null</summary>
    public static OslIdentity? ParseIdentity(ReadOnlySpan<byte> b)
    {
      if (b.Length < OslRegister.IdentLength || b[0] != 'O' || b[1] != 'S' || b[2] != 'L') return null;
      if (!OslCrc.Check(b[..OslRegister.IdentLength])) return null;
      return new OslIdentity(
        Spec: b[3],
        Vendor: BinaryPrimitives.ReadUInt16LittleEndian(b[4..]),
        Model: BinaryPrimitives.ReadUInt16LittleEndian(b[6..]),
        Firmware: $"{b[8]}.{b[9]}.{b[10]}",
        ChannelCount: b[11],
        PeriodMs: BinaryPrimitives.ReadUInt16LittleEndian(b[12..]),
        DeviceId: b.Slice(14, 16).ToArray());
    }

    public async Task<string> ReadNameAsync(CancellationToken ct = default)
    {
      var b = await ReadCheckedAsync(OslRegister.Name, OslRegister.NameLength + 2, ct).ConfigureAwait(false);
      return CString(b.AsSpan(0, OslRegister.NameLength));
    }

    /// <summary>名前を書く (UTF-8、63 バイトまで)。書いた後に読み直して確かめる</summary>
    public async Task<bool> SetNameAsync(string text, CancellationToken ct = default)
    {
      var raw = Encoding.UTF8.GetBytes(text);
      if (raw.Length > OslRegister.NameLength - 1)
        throw new ArgumentException($"名前は UTF-8 で {OslRegister.NameLength - 1} バイトまで (現在 {raw.Length} バイト)");
      var buf = new byte[OslRegister.NameLength];
      raw.CopyTo(buf, 0);
      if (!await WriteRegAsync(OslRegister.Name, buf, ct).ConfigureAwait(false)) return false;
      Name = await ReadNameAsync(ct).ConfigureAwait(false);
      return Name == text;
    }

    private async Task<IReadOnlyList<OslChannel>> ReadChannelsAsync(CancellationToken ct)
    {
      int n = Identity.ChannelCount;
      if (n == 0) return Array.Empty<OslChannel>();
      var b = await ReadCheckedAsync(OslRegister.ChDesc, 16 * n + 2, ct).ConfigureAwait(false);

      byte[]? attr = null;
      if (ExtIdentity?.Capability.HasFlag(OslCapability.ChannelAttributes) == true)
        attr = await ReadCheckedAsync(OslRegister.ExtAttr, 4 * n + 2, ct).ConfigureAwait(false);

      var list = new List<OslChannel>(n);
      for (int i = 0; i < n; i++)
      {
        var d = b.AsSpan(16 * i, 16);
        list.Add(new OslChannel(
          Index: i,
          Name: CString(d[4..]),
          Unit: BinaryPrimitives.ReadUInt16LittleEndian(d),
          Type: (OslValueType)d[2],
          Precision: attr?[4 * i] ?? 0,
          Flags: (OslChannelFlags)(attr?[4 * i + 1] ?? 0)));
      }
      return list;
    }

    /// <summary>POLL の要求 (番地 0x0300 から <see cref="PollLength"/> バイト)</summary>
    public (byte Addr, byte[] Write, int ReadLength) PollRequest => (Address, RegBytes(OslRegister.Poll), PollLength);

    public async Task<OslSample?> PollAsync(CancellationToken ct = default, int tries = 2)
    {
      for (int i = 0; i < tries; i++)
      {
        var s = ParsePoll(await ReadRegAsync(OslRegister.Poll, PollLength, ct).ConfigureAwait(false));
        if (s != null) return s;
      }
      return null;
    }

    /// <summary>POLL 区画を解釈する。長さか CRC が合わなければ null</summary>
    public OslSample? ParsePoll(byte[]? b)
    {
      if (b == null || b.Length != PollLength || !OslCrc.Check(b)) return null;
      int n = Channels.Count;
      var values = new double[n];
      for (int i = 0; i < n; i++)
      {
        var v = b.AsSpan(2 + 4 * i, 4);
        values[i] = Channels[i].Type switch
        {
          OslValueType.Int32 => BinaryPrimitives.ReadInt32LittleEndian(v),
          OslValueType.UInt32 => BinaryPrimitives.ReadUInt32LittleEndian(v),
          _ => BinaryPrimitives.ReadSingleLittleEndian(v),
        };
      }
      return new OslSample(
        Seq: BinaryPrimitives.ReadUInt16LittleEndian(b),
        Values: values,
        Status: b.AsSpan(2 + 4 * n, n).ToArray());
    }

    /// <summary>I²C アドレスを変える。子機は STOP の直後から新アドレスで応答する</summary>
    public async Task<bool> SetAddressAsync(byte newAddress, CancellationToken ct = default)
    {
      if (newAddress is < OslRegister.MinAddress or > OslRegister.MaxAddress)
        throw new ArgumentOutOfRangeException(nameof(newAddress), "アドレスは 0x08〜0x77");
      if (!await WriteRegAsync(OslRegister.AddrKey, new[] { OslRegister.Key, newAddress }, ct).ConfigureAwait(false))
        return false;
      byte old = Address;
      Address = newAddress;
      // 新アドレスで識別が読めることを確かめる
      var b = await _bus.ReadAsync(Address, OslRegister.IdentLength, ct).ConfigureAwait(false);
      if (b != null && ParseIdentity(b)?.Uid == Identity.Uid) return true;
      Address = old;
      return false;
    }

    #endregion

    #region 任意区画

    private async Task<OslExtIdentity?> ReadExtIdentityAsync(CancellationToken ct)
    {
      var b = await ReadRegAsync(OslRegister.ExtIdent, 10, ct).ConfigureAwait(false)
        ?? throw new OslException($"0x{Address:X2}: 拡張識別を読めない");
      if (OslCrc.IsUnimplemented(b)) return null;
      if (!OslCrc.Check(b))
      {
        b = await ReadCheckedAsync(OslRegister.ExtIdent, 10, ct).ConfigureAwait(false);
        if (OslCrc.IsUnimplemented(b)) return null;
      }
      return new OslExtIdentity(
        Capability: (OslCapability)BinaryPrimitives.ReadUInt16LittleEndian(b),
        HwRev: b[2],
        AvgWindowMs: BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(4)),
        WarmupMs: BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(6)));
    }

    /// <summary>拡張ステータス。未実装なら null</summary>
    public async Task<OslExtStatus?> ReadExtStatusAsync(CancellationToken ct = default)
    {
      if (!HasExtStatus) return null;
      var b = await ReadCheckedAsync(OslRegister.ExtStatus, 12, ct).ConfigureAwait(false);
      return new OslExtStatus(
        UptimeSec: BinaryPrimitives.ReadUInt32LittleEndian(b),
        NodeFlags: b[4],
        ResetCause: b[5],
        ErrI2c: BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(6)),
        ErrSensor: BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(8)));
    }

    /// <summary>識別のため LED を点滅させる (対応している子機のみ)</summary>
    public Task<bool> IdentifyAsync(int seconds, CancellationToken ct = default)
      => WriteRegAsync(OslRegister.CmdKey, new[] { OslRegister.Key, OslRegister.CmdIdentify, (byte)Math.Clamp(seconds, 0, 255) }, ct);

    #endregion
  }
}
