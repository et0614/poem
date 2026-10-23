using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PoEMLink.App.Services;
using PoEMLink.Link;

namespace PoEMLink.App.ViewModels
{
  /// <summary>Link の情報・バスの状態と、アプリの設定 (記録の間隔、模擬モード)</summary>
  public partial class SettingsViewModel : ObservableObject
  {
    private readonly LinkSession _session;
    private bool _updating;

    public MainViewModel Main { get; }

    [ObservableProperty]
    private IReadOnlyList<InfoRow> linkRows = Array.Empty<InfoRow>();

    [ObservableProperty]
    private IReadOnlyList<InfoRow> busRows = Array.Empty<InfoRow>();

    [ObservableProperty]
    private bool isConnected;

    [ObservableProperty]
    private bool isDemo;

    public IReadOnlyList<int> RecordIntervals { get; } = new[] { 1, 2, 5, 10, 30, 60 };

    public string AppVersion => $"PoEM Link {AppInfo.Current.Version.ToString(3)} (build {AppInfo.Current.BuildString})";

    public SettingsViewModel(LinkSession session, MainViewModel main)
    {
      _session = session;
      Main = main;
      _session.StateChanged += Refresh;
      Refresh();
    }

    private void Refresh()
    {
      _updating = true;
      IsConnected = _session.IsReady;
      IsDemo = _session.IsDemo;
      _updating = false;

      var info = _session.Info;
      LinkRows = info == null
        ? new[] { new InfoRow("接続", _session.LastError ?? "未接続") }
        : new[]
        {
          new InfoRow("機器", _session.Active?.DeviceName ?? "PoEM Link"),
          new InfoRow("ファームウェア", $"{info.Firmware} (プロトコル {info.ProtocolVersion})"),
          new InfoRow("固有番号", info.SerialHex),
          new InfoRow("I²C の速度", $"{info.SpeedKHz} kHz"),
          new InfoRow("タイムアウト", $"{info.TimeoutMs} ms"),
          new InfoRow("転送の上限", $"{info.MaxTransfer} B (BATCH {info.MaxBatch} B)"),
        };
      BusRows = Array.Empty<InfoRow>();
    }

    partial void OnIsDemoChanged(bool value)
    {
      if (_updating) return;
      _ = _session.SetDemoAsync(value);
    }

    [RelayCommand]
    private async Task LoadBusStatusAsync()
    {
      if (_session.Client == null) return;
      try
      {
        var s = await _session.Client.GetBusStatusAsync();
        BusRows = new[]
        {
          new InfoRow("バス", s.BusStateName),
          new InfoRow("SDA / SCL", $"{(s.Sda ? "High" : "Low")} / {(s.Scl ? "High" : "Low")}"),
          new InfoRow("タイムアウト", s.Timeouts.ToString()),
          new InfoRow("NACK", s.Nacks.ToString()),
          new InfoRow("使用中で開始できず", s.BusBusy.ToString()),
          new InfoRow("調停負け", s.ArbLost.ToString()),
          new InfoRow("復帰の実行", s.Recoveries.ToString()),
        };
      }
      catch (LinkException ex)
      {
        BusRows = new[] { new InfoRow("バスの状態", ex.Message) };
      }
    }

    [RelayCommand]
    private async Task RecoverBusAsync()
    {
      if (_session.Client == null) return;
      try
      {
        var r = await _session.Client.BusRecoverAsync();
        await Shell.Current.DisplayAlertAsync("バスの復帰", r == LinkResult.Ok ? "復帰しました" : "復帰できませんでした (SDA か SCL が Low のまま)", "OK");
        await LoadBusStatusAsync();
      }
      catch (LinkException ex)
      {
        await Shell.Current.DisplayAlertAsync("バスの復帰", ex.Message, "OK");
      }
    }

    [RelayCommand]
    private async Task IdentifyLinkAsync()
    {
      if (_session.Client == null) return;
      try
      {
        await _session.Client.IdentifyAsync(5);
      }
      catch (LinkException ex)
      {
        await Shell.Current.DisplayAlertAsync("識別", ex.Message, "OK");
      }
    }
  }
}
