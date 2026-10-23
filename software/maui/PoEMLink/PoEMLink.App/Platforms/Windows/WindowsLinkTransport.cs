using Melanchall.DryWetMidi.Core;
using Melanchall.DryWetMidi.Multimedia;
using PoEMLink.Midi;

namespace PoEMLink.App.Platforms.Windows
{
  /// <summary>
  /// DryWetMidi で PoEM Link と SysEx をやり取りする (E-Sensor の WindowsMidiService を改修)。
  /// Windows は抜き差しの通知が無いので、1 秒ごとに機器の一覧を確かめる。
  /// </summary>
  public class WindowsLinkTransport : ILinkTransport, IDisposable
  {
    private const string TargetDeviceName = "PoEM Link";

    private readonly System.Timers.Timer _deviceCheckTimer;
    private IInputDevice? _input;
    private IOutputDevice? _output;
    private int _checking;

    public bool IsConnected { get; private set; }

    public string? DeviceName { get; private set; }

    public event Action<byte[]>? SysExReceived;
    public event Action<bool>? ConnectionChanged;

    public WindowsLinkTransport()
    {
      _deviceCheckTimer = new System.Timers.Timer(1000) { AutoReset = true };
      _deviceCheckTimer.Elapsed += (_, _) => CheckDevices();
      _deviceCheckTimer.Start();
      Task.Run(CheckDevices);
    }

    private void CheckDevices()
    {
      if (Interlocked.Exchange(ref _checking, 1) == 1) return;
      try
      {
        bool present = InputDevice.GetAll().Any(d => d.Name.Contains(TargetDeviceName, StringComparison.OrdinalIgnoreCase));
        if (!IsConnected && present) TryConnect();
        else if (IsConnected && !present) Close();
      }
      catch (Exception ex)
      {
        System.Diagnostics.Debug.WriteLine($"[MIDI] device check failed: {ex.Message}");
      }
      finally
      {
        Interlocked.Exchange(ref _checking, 0);
      }
    }

    private void TryConnect()
    {
      try
      {
        var input = InputDevice.GetAll().FirstOrDefault(d => d.Name.Contains(TargetDeviceName, StringComparison.OrdinalIgnoreCase));
        var output = OutputDevice.GetAll().FirstOrDefault(d => d.Name.Contains(TargetDeviceName, StringComparison.OrdinalIgnoreCase));
        if (input == null || output == null) return;

        _input = input;
        _output = output;
        _output.PrepareForEventsSending();
        _input.EventReceived += OnMidiEventReceived;
        _input.StartEventsListening();
        DeviceName = input.Name;
        IsConnected = true;
        ConnectionChanged?.Invoke(true);
      }
      catch (Exception ex)
      {
        System.Diagnostics.Debug.WriteLine($"[MIDI] connection failed: {ex.Message}");
        Close();
      }
    }

    private void OnMidiEventReceived(object? sender, MidiEventReceivedEventArgs e)
    {
      // DryWetMidi の SysEx のデータは F0 を含まず、末尾に F7 を含む
      if (e.Event is SysExEvent sysEx)
      {
        var d = sysEx.Data;
        int n = d.Length > 0 && d[^1] == 0xF7 ? d.Length - 1 : d.Length;
        if (n > 0) SysExReceived?.Invoke(d.AsSpan(0, n).ToArray());
      }
    }

    public void SendSysEx(byte[] body)
    {
      if (!IsConnected || _output == null) return;
      var data = new byte[body.Length + 1];
      body.CopyTo(data, 0);
      data[^1] = 0xF7;
      try
      {
        _output.SendEvent(new NormalSysExEvent(data));
      }
      catch (Exception ex)
      {
        System.Diagnostics.Debug.WriteLine($"[MIDI] send error: {ex.Message}");
      }
    }

    private void Close()
    {
      bool was = IsConnected;
      IsConnected = false;
      DeviceName = null;
      if (_input != null)
      {
        _input.EventReceived -= OnMidiEventReceived;
        _input.StopEventsListening();
        (_input as IDisposable)?.Dispose();
        _input = null;
      }
      (_output as IDisposable)?.Dispose();
      _output = null;
      if (was) ConnectionChanged?.Invoke(false);
    }

    public void Dispose()
    {
      _deviceCheckTimer.Stop();
      _deviceCheckTimer.Dispose();
      Close();
    }
  }
}
