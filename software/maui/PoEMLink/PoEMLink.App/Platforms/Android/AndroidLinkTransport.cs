using System.Runtime.Versioning;
using Android.Content;
using Android.Media.Midi;
using Android.OS;
using PoEMLink.Midi;

namespace PoEMLink.App.Platforms.Android
{
  /// <summary>
  /// Android の MIDI API で PoEM Link と SysEx をやり取りする (E-Sensor の AndroidMidiService を改修)。
  /// </summary>
  [SupportedOSPlatform("android26.0")]
  internal class AndroidLinkTransport : Java.Lang.Object, ILinkTransport, MidiManager.IOnDeviceOpenedListener
  {
    private const string TargetDeviceName = "PoEM Link";

    private readonly MidiManager _midiManager;
    private readonly MidiDeviceCallback _deviceCallback;
    private MidiDevice? _midiDevice;
    private MidiInputPort? _inputPort;
    private MidiOutputPort? _outputPort;
    private bool _isConnecting;

    public bool IsConnected { get; private set; }

    public string? DeviceName { get; private set; }

    public event Action<byte[]>? SysExReceived;
    public event Action<bool>? ConnectionChanged;

    public AndroidLinkTransport()
    {
      _midiManager = (MidiManager)Platform.AppContext.GetSystemService(Context.MidiService)!;
      _deviceCallback = new MidiDeviceCallback(this);
#pragma warning disable CA1422
      _midiManager.RegisterDeviceCallback(_deviceCallback, new Handler(Looper.MainLooper!));
#pragma warning restore CA1422
      TryConnect();
    }

    private static string? NameOf(MidiDeviceInfo? info) => info?.Properties?.GetString(MidiDeviceInfo.PropertyName);

    private void TryConnect()
    {
      if (IsConnected || _isConnecting) return;
      try
      {
#pragma warning disable CA1422
        var info = (_midiManager.GetDevices() ?? Array.Empty<MidiDeviceInfo>())
#pragma warning restore CA1422
          .FirstOrDefault(d => NameOf(d)?.Contains(TargetDeviceName, StringComparison.OrdinalIgnoreCase) == true);
        if (info == null) return;
        _isConnecting = true;
        DeviceName = NameOf(info);
        _midiManager.OpenDevice(info, this, new Handler(Looper.MainLooper!));
      }
      catch (Exception ex)
      {
        _isConnecting = false;
        System.Diagnostics.Debug.WriteLine($"[MIDI] Android connection error: {ex.Message}");
      }
    }

    public void OnDeviceOpened(MidiDevice? device)
    {
      _isConnecting = false;
      if (device == null) return;
      _midiDevice = device;
      _inputPort = device.OpenInputPort(0);
      _outputPort = device.OpenOutputPort(0);
      if (_inputPort == null || _outputPort == null)
      {
        Close();
        return;
      }
      var receiver = new SysExReceiver();
      receiver.Received += body => SysExReceived?.Invoke(body);
      _outputPort.Connect(receiver);
      IsConnected = true;
      ConnectionChanged?.Invoke(true);
    }

    public void SendSysEx(byte[] body)
    {
      if (!IsConnected || _inputPort == null) return;
      var raw = new byte[body.Length + 2];
      raw[0] = 0xF0;
      body.CopyTo(raw, 1);
      raw[^1] = 0xF7;
      _inputPort.Send(raw, 0, raw.Length);
    }

    private void Close()
    {
      bool was = IsConnected;
      IsConnected = false;
      DeviceName = null;
      _inputPort?.Close();
      _outputPort?.Close();
      _midiDevice?.Close();
      _inputPort = null;
      _outputPort = null;
      _midiDevice = null;
      if (was) ConnectionChanged?.Invoke(false);
    }

    protected override void Dispose(bool disposing)
    {
      if (disposing)
      {
        _midiManager.UnregisterDeviceCallback(_deviceCallback);
        Close();
      }
      base.Dispose(disposing);
    }

    private class MidiDeviceCallback : MidiManager.DeviceCallback
    {
      private readonly AndroidLinkTransport _parent;

      public MidiDeviceCallback(AndroidLinkTransport parent) => _parent = parent;

      public override void OnDeviceAdded(MidiDeviceInfo? device)
      {
        if (NameOf(device)?.Contains(TargetDeviceName, StringComparison.OrdinalIgnoreCase) == true)
          _parent.TryConnect();
      }

      public override void OnDeviceRemoved(MidiDeviceInfo? device)
      {
        if (NameOf(device)?.Contains(TargetDeviceName, StringComparison.OrdinalIgnoreCase) == true)
          _parent.Close();
      }
    }

    /// <summary>分かれて届く MIDI のバイト列から SysEx を組み立てる</summary>
    private class SysExReceiver : MidiReceiver
    {
      private readonly List<byte> _buffer = new();
      private bool _inSysEx;

      public event Action<byte[]>? Received;

      public override void OnSend(byte[]? msg, int offset, int count, long timestamp)
      {
        if (msg == null) return;
        for (int i = 0; i < count; i++)
        {
          byte b = msg[offset + i];
          if (b == 0xF0) { _buffer.Clear(); _inSysEx = true; continue; }
          if (b == 0xF7)
          {
            if (_inSysEx && _buffer.Count > 0) Received?.Invoke(_buffer.ToArray());
            _buffer.Clear();
            _inSysEx = false;
            continue;
          }
          if (_inSysEx && b < 0x80) _buffer.Add(b);
        }
      }
    }
  }
}
