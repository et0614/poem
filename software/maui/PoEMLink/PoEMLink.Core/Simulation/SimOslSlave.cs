using System.Buffers.Binary;
using System.Diagnostics;
using System.Text;
using PoEMLink.Osl;

namespace PoEMLink.Simulation
{
  /// <summary>模擬の I²C 機器。1 回の転送は Start → Write/Read → Stop の順に呼ばれる</summary>
  public interface ISimI2cDevice
  {
    byte Address { get; }

    void Start();

    /// <summary>書く。ACK したバイト数を返す</summary>
    int Write(ReadOnlySpan<byte> data);

    byte[] Read(int n);

    void Stop();
  }

  /// <summary>模擬の子機のチャネル</summary>
  public sealed record SimChannel(ushort Unit, OslValueType Type, string Name, byte Precision, OslChannelFlags Flags);

  /// <summary>
  /// OSL v2 子機の模擬 (firmware/osl_common/osl_slave.c と同じ規則)。
  ///
  /// - 16bit 番地は上位→下位の順で受け取る。番地だけを書いた転送の後は読み出し位置を保ち、
  ///   それ以外の転送の後は STOP で読み出し位置を 0x0000 に戻す。
  /// - 番地 0x0300 (POLL) と 0x0480 (拡張ステータス) を設定した時点の値を固定して返す。
  /// - 名前・アドレス変更・コマンドは STOP で反映する。
  /// </summary>
  public sealed class SimOslSlave : ISimI2cDevice
  {
    private readonly ushort _model;
    private readonly byte[] _fw;
    private readonly ushort _periodMs;
    private readonly IReadOnlyList<SimChannel> _channels;
    private readonly OslCapability _capability;
    private readonly ushort _warmupMs;
    private readonly ushort _avgWindowMs;
    private readonly byte[] _deviceId;
    private readonly Func<double, int, (double Value, byte Status)> _generator;
    private readonly Stopwatch _clock = Stopwatch.StartNew();

    private byte[] _name = new byte[OslRegister.NameLength];
    private byte _resetCause = 1;

    // 転送中の状態
    private ushort _ptr;
    private int _addrBytes;
    private bool _wasRead, _wrote;
    private byte _addrKey, _cmdKey, _cmd, _cmdArg;
    private byte? _newAddr;
    private byte[]? _nameStage;
    private byte[] _pollShadow = Array.Empty<byte>();
    private byte[] _extStatusShadow = Array.Empty<byte>();

    public byte Address { get; private set; }

    /// <summary>識別点滅の残り時間 (アプリの表示確認用)</summary>
    public DateTime IdentifyUntil { get; private set; }

    /// <param name="generator">(経過秒, チャネル番号) → (値, 状態)</param>
    public SimOslSlave(byte address, ushort model, string name, ushort periodMs, IReadOnlyList<SimChannel> channels,
      OslCapability capability, ushort warmupMs, Func<double, int, (double, byte)> generator, string firmware = "2.0.0",
      ushort? avgWindowMs = null)
    {
      Address = address;
      _model = model;
      _periodMs = periodMs;
      _channels = channels;
      _capability = capability;
      _warmupMs = warmupMs;
      _avgWindowMs = avgWindowMs ?? warmupMs;
      _generator = generator;
      _fw = firmware.Split('.').Select(byte.Parse).ToArray();
      _deviceId = new byte[16];
      new Random(address * 7919 + model).NextBytes(_deviceId);
      Encoding.UTF8.GetBytes(name).AsSpan(0, Math.Min(63, Encoding.UTF8.GetByteCount(name))).CopyTo(_name);
    }

    public double ElapsedSec => _clock.Elapsed.TotalSeconds;

    #region 区画

    private static byte[] WithCrc(byte[] b)
    {
      var r = new byte[b.Length + 2];
      b.CopyTo(r, 0);
      BinaryPrimitives.WriteUInt16LittleEndian(r.AsSpan(b.Length), OslCrc.Compute(b));
      return r;
    }

    private byte[] IdentBlock()
    {
      var b = new byte[30];
      "OSL"u8.CopyTo(b);
      b[3] = 2;
      BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(4), 0x0001);
      BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(6), _model);
      b[8] = _fw[0]; b[9] = _fw[1]; b[10] = _fw[2];
      b[11] = (byte)_channels.Count;
      BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(12), _periodMs);
      _deviceId.CopyTo(b, 14);
      return WithCrc(b);
    }

    private byte[] NameBlock() => WithCrc(_name);

    private byte[] ChDescBlock()
    {
      var b = new byte[16 * _channels.Count];
      for (int i = 0; i < _channels.Count; i++)
      {
        var c = _channels[i];
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(16 * i), c.Unit);
        b[16 * i + 2] = (byte)c.Type;
        var nm = Encoding.UTF8.GetBytes(c.Name);
        nm.AsSpan(0, Math.Min(11, nm.Length)).CopyTo(b.AsSpan(16 * i + 4));
      }
      return WithCrc(b);
    }

    private byte[] PollLive()
    {
      int n = _channels.Count;
      var b = new byte[2 + 5 * n + (n & 1)];
      double t = ElapsedSec;
      BinaryPrimitives.WriteUInt16LittleEndian(b, (ushort)(t * 1000 / _periodMs));
      for (int i = 0; i < n; i++)
      {
        var (v, st) = t * 1000 < _warmupMs ? (0.0, (byte)64) : _generator(t, i);
        var span = b.AsSpan(2 + 4 * i, 4);
        switch (_channels[i].Type)
        {
          case OslValueType.Int32: BinaryPrimitives.WriteInt32LittleEndian(span, (int)v); break;
          case OslValueType.UInt32: BinaryPrimitives.WriteUInt32LittleEndian(span, (uint)v); break;
          default: BinaryPrimitives.WriteSingleLittleEndian(span, (float)v); break;
        }
        b[2 + 4 * n + i] = st;
      }
      return WithCrc(b);
    }

    private byte[] ExtIdentBlock()
    {
      var b = new byte[8];
      BinaryPrimitives.WriteUInt16LittleEndian(b, (ushort)_capability);
      BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(4), _avgWindowMs);
      BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(6), _warmupMs);
      return WithCrc(b);
    }

    private byte[] ExtAttrBlock()
    {
      var b = new byte[4 * _channels.Count];
      for (int i = 0; i < _channels.Count; i++)
      {
        b[4 * i] = _channels[i].Precision;
        b[4 * i + 1] = (byte)_channels[i].Flags;
      }
      return WithCrc(b);
    }

    private byte[] ExtStatusLive()
    {
      var b = new byte[10];
      BinaryPrimitives.WriteUInt32LittleEndian(b, (uint)ElapsedSec);
      b[5] = _resetCause;
      return WithCrc(b);
    }

    private byte ByteAt(ushort a)
    {
      (ushort Base, Func<byte[]> Block)[] regions =
      {
        (OslRegister.Ident, IdentBlock),
        (OslRegister.Name, NameBlock),
        (OslRegister.ChDesc, ChDescBlock),
        (OslRegister.Poll, () => _pollShadow),
        (OslRegister.ExtIdent, ExtIdentBlock),
        (OslRegister.ExtAttr, ExtAttrBlock),
        (OslRegister.ExtStatus, () => _extStatusShadow),
      };
      foreach (var (bas, block) in regions)
      {
        if (a < bas) continue;
        var blk = block();
        if (a < bas + blk.Length) return blk[a - bas];
      }
      return 0;
    }

    #endregion

    #region 転送

    public void Start()
    {
      _addrBytes = 0;
      _wasRead = _wrote = false;
      _addrKey = _cmdKey = _cmd = _cmdArg = 0;
      _newAddr = null;
      _nameStage = null;
    }

    public int Write(ReadOnlySpan<byte> data)
    {
      foreach (var b in data)
      {
        if (_addrBytes == 0)
        {
          _ptr = (ushort)(b << 8);
          _addrBytes = 1;
        }
        else if (_addrBytes == 1)
        {
          _ptr |= b;
          _addrBytes = 2;
          if (_ptr == OslRegister.Poll) _pollShadow = PollLive();
          if (_ptr == OslRegister.ExtStatus) _extStatusShadow = ExtStatusLive();
        }
        else
        {
          ushort a = _ptr++;
          _wrote = true;
          if (a == OslRegister.AddrKey) _addrKey = b;
          else if (a == OslRegister.AddrKey + 1) _newAddr = b;
          else if (a >= OslRegister.Name && a < OslRegister.Name + OslRegister.NameLength)
          {
            _nameStage ??= (byte[])_name.Clone();
            _nameStage[a - OslRegister.Name] = b;
          }
          else if (a == OslRegister.CmdKey) _cmdKey = b;
          else if (a == OslRegister.CmdKey + 1) _cmd = b;
          else if (a == OslRegister.CmdKey + 2) _cmdArg = b;
        }
      }
      return data.Length;
    }

    public byte[] Read(int n)
    {
      _wasRead = true;
      var r = new byte[n];
      for (int i = 0; i < n; i++) r[i] = ByteAt(_ptr++);
      return r;
    }

    public void Stop()
    {
      if (_addrKey == OslRegister.Key && _newAddr is >= OslRegister.MinAddress and <= OslRegister.MaxAddress)
        Address = _newAddr.Value;
      if (_nameStage != null)
      {
        _nameStage[^1] = 0;
        _name = _nameStage;
      }
      if (_cmdKey == OslRegister.Key)
      {
        if (_cmd == OslRegister.CmdReset)
        {
          _clock.Restart();
          _resetCause = 4;
        }
        else if (_cmd == OslRegister.CmdIdentify && _capability.HasFlag(OslCapability.CmdIdentify))
        {
          IdentifyUntil = DateTime.Now.AddSeconds(_cmdArg);
        }
      }
      if (_wasRead || _wrote || _addrBytes != 2) _ptr = 0;
    }

    #endregion
  }

  /// <summary>OSL ではない I²C 機器の模擬 (読むと 0xFF、書き込みは受け流す)</summary>
  public sealed class SimForeignDevice : ISimI2cDevice
  {
    public byte Address { get; }

    public SimForeignDevice(byte address) => Address = address;

    public void Start() { }

    public int Write(ReadOnlySpan<byte> data) => data.Length;

    public byte[] Read(int n) => Enumerable.Repeat((byte)0xFF, n).ToArray();

    public void Stop() { }
  }
}
