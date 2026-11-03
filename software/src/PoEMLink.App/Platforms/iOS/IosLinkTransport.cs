using AVFoundation;
using CoreMidi;
using PoEMLink.Midi;

namespace PoEMLink.App.Platforms.iOS
{
  /// <summary>
  /// CoreMIDI で PoEM Link と SysEx をやり取りする (E-Sensor の IosMidiService を改修)。
  /// 中身の符号化は PoemLinkClient が行うので、ここでは F0 / F7 の付け外しだけをする。
  /// </summary>
  public class IosLinkTransport : ILinkTransport, IDisposable
  {
    private const string TargetDeviceName = "PoEM Link";

    private MidiClient? _client;
    private MidiPort? _inputPort;
    private MidiPort? _outputPort;
    private MidiEndpoint? _source;
    private MidiEndpoint? _destination;
    private readonly List<byte> _sysExBuffer = new();
    private bool _inSysEx;

    public bool IsConnected { get; private set; }

    public string? DeviceName { get; private set; }

    public event Action<byte[]>? SysExReceived;
    public event Action<bool>? ConnectionChanged;

    public IosLinkTransport()
    {
      // 記録中にバックグラウンドへ回っても止まらないよう、他のアプリの音を邪魔しない
      // 再生カテゴリの音声セッションを有効にしておく (E-Sensor と同じ)
      var session = AVAudioSession.SharedInstance();
      session.SetCategory(AVAudioSessionCategory.Playback, AVAudioSessionCategoryOptions.MixWithOthers);
      session.SetActive(true);

      _client = new MidiClient(TargetDeviceName);
      _client.ObjectAdded += (_, _) => { if (!IsConnected) TryConnect(); };
      _client.ObjectRemoved += OnMidiObjectRemoved;

      _inputPort = _client.CreateInputPort(TargetDeviceName + " Input");
      if (_inputPort != null) _inputPort.MessageReceived += OnMidiMessageReceived;
      _outputPort = _client.CreateOutputPort(TargetDeviceName + " Output");

      TryConnect();
    }

    private void OnMidiObjectRemoved(object? sender, ObjectAddedOrRemovedEventArgs e)
    {
      if (IsConnected && e.Child?.Handle == _source?.Handle) CloseConnectionOnly();
    }

    private void TryConnect()
    {
      if (IsConnected) return;

      for (int i = 0; i < (int)CoreMidi.Midi.SourceCount; i++)
      {
        var ep = MidiEndpoint.GetSource(i);
        if (ep?.Name?.Contains(TargetDeviceName, StringComparison.OrdinalIgnoreCase) == true)
        {
          _source = ep;
          _inputPort?.ConnectSource(_source);
          break;
        }
      }
      for (int i = 0; i < (int)CoreMidi.Midi.DestinationCount; i++)
      {
        var ep = MidiEndpoint.GetDestination(i);
        if (ep?.Name?.Contains(TargetDeviceName, StringComparison.OrdinalIgnoreCase) == true)
        {
          _destination = ep;
          DeviceName = ep.Name;
          IsConnected = _source != null;
          if (IsConnected) ConnectionChanged?.Invoke(true);
          break;
        }
      }
    }

    private void OnMidiMessageReceived(object? sender, MidiPacketsEventArgs e)
    {
      foreach (var packet in e.Packets)
      {
        var buffer = packet.ByteArray;
        if (buffer == null || buffer.Length == 0) continue;
        foreach (var b in buffer)
        {
          if (b == 0xF0) { _sysExBuffer.Clear(); _inSysEx = true; continue; }
          if (b == 0xF7)
          {
            if (_inSysEx && _sysExBuffer.Count > 0) SysExReceived?.Invoke(_sysExBuffer.ToArray());
            _sysExBuffer.Clear();
            _inSysEx = false;
            continue;
          }
          if (_inSysEx && b < 0x80) _sysExBuffer.Add(b);
        }
      }
    }

    public void SendSysEx(byte[] body)
    {
      if (!IsConnected || _outputPort == null || _destination == null) return;
      var raw = new byte[body.Length + 2];
      raw[0] = 0xF0;
      body.CopyTo(raw, 1);
      raw[^1] = 0xF7;
      _outputPort.Send(_destination, new[] { new MidiPacket(0, raw, 0, raw.Length) });
    }

    private void CloseConnectionOnly()
    {
      IsConnected = false;
      DeviceName = null;
      ConnectionChanged?.Invoke(false);
      if (_source != null)
      {
        _inputPort?.Disconnect(_source);
        _source.Dispose();
        _source = null;
      }
      _destination?.Dispose();
      _destination = null;
    }

    public void Dispose()
    {
      CloseConnectionOnly();
      _inputPort?.Dispose();
      _outputPort?.Dispose();
      _client?.Dispose();
      _inputPort = _outputPort = null;
      _client = null;
    }
  }
}
