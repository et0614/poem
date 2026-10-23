using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PoEMLink.App.Services;
using PoEMLink.Recording;
using PoEMLink.Link;
using PoEMLink.Osl;

namespace PoEMLink.App.ViewModels
{
  /// <summary>
  /// 計測画面。Link がつながったら子機を走査し、子機ごとの周期で POLL を読み続ける。
  ///
  /// - 読み取りの繰り返しは UI のスレッドで回す (await の間は UI を止めない)。子機の一覧を
  ///   別のスレッドから触らずに済む。Link への要求は PoemLinkClient が 1 つずつに並べる。
  /// - 数秒ごとにアドレスを走査し直し、子機の抜き差しとアドレスの変化を反映する。
  /// </summary>
  public partial class MainViewModel : ObservableObject
  {
    /// <summary>走査し直す間隔</summary>
    private static readonly TimeSpan RescanInterval = TimeSpan.FromSeconds(3);

    private readonly LinkSession _session;
    private readonly Recorder _recorder;
    private readonly IBackgroundService _background;
    private CancellationTokenSource? _loopCts;
    private CancellationTokenSource? _recordCts;
    private Task _loop = Task.CompletedTask;

    public ObservableCollection<NodeViewModel> Nodes { get; } = new();

    [ObservableProperty]
    private bool isConnected;

    [ObservableProperty]
    private bool isDemo;

    [ObservableProperty]
    private string connectionText = "PoEM Link を接続してください";

    [ObservableProperty]
    private string unknownText = "";

    [ObservableProperty]
    private string errorText = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(RecordButtonText))]
    private bool isRecording;

    [ObservableProperty]
    private string recordText = "";

    [ObservableProperty]
    private bool hasRecord;

    /// <summary>記録の間隔 [s]</summary>
    [ObservableProperty]
    private int recordIntervalSec = 1;

    public string RecordButtonText => IsRecording ? "記録停止" : "記録開始";

    public bool IsEmpty => IsConnected && Nodes.Count == 0;

    public MainViewModel(LinkSession session, Recorder recorder, IBackgroundService background)
    {
      _session = session;
      _recorder = recorder;
      _background = background;
      _session.StateChanged += OnSessionChanged;
      Nodes.CollectionChanged += (_, _) => OnPropertyChanged(nameof(IsEmpty));
    }

    #region 接続と読み取り

    private void OnSessionChanged()
    {
      IsConnected = _session.IsReady;
      IsDemo = _session.IsDemo;
      OnPropertyChanged(nameof(IsEmpty));

      _loopCts?.Cancel();
      _loopCts = null;

      if (_session.IsReady)
      {
        ConnectionText = _session.IsDemo
          ? "模擬モード (実機ではありません)"
          : $"{_session.Active?.DeviceName ?? "PoEM Link"}  fw {_session.Info?.Firmware}";
        ErrorText = "";
        Nodes.Clear();
        UnknownText = "";
        _loopCts = new CancellationTokenSource();
        _loop = RunLoopAsync(_session.Bus!, _loopCts.Token);
      }
      else
      {
        ConnectionText = "PoEM Link を接続してください";
        ErrorText = _session.LastError ?? "";
        UnknownText = "";
        // 記録中は子機を通信失敗として残す (記録の列を保つため)。記録していなければ一覧を消す
        if (IsRecording)
          foreach (var n in Nodes) n.MarkOffline();
        else
          Nodes.Clear();
      }
    }

    private async Task RunLoopAsync(II2cBus bus, CancellationToken ct)
    {
      var lastScan = DateTime.MinValue;
      while (!ct.IsCancellationRequested)
      {
        try
        {
          var now = DateTime.Now;
          if (now - lastScan >= RescanInterval)
          {
            await RescanAsync(bus, ct);
            lastScan = DateTime.Now;
          }

          var due = Nodes.Where(n => n.IsOnline && n.NextDue <= now).ToList();
          if (due.Count > 0)
          {
            var raw = await bus.WriteReadManyAsync(due.Select(n => n.Device.PollRequest).ToList(), ct);
            for (int i = 0; i < due.Count; i++)
            {
              due[i].Apply(due[i].Device.ParsePoll(raw[i]));
              due[i].NextDue = now + due[i].PollInterval;
            }
          }
          ErrorText = "";
          await Task.Delay(50, ct);
        }
        catch (OperationCanceledException)
        {
          break;
        }
        catch (Exception ex) when (ex is LinkException or OslException)
        {
          // 応答が途切れても繰り返しは止めない (抜かれた場合は接続の切断で止まる)
          ErrorText = ex.Message;
          try { await Task.Delay(1000, ct); } catch (OperationCanceledException) { break; }
        }
      }
    }

    /// <summary>
    /// アドレスを走査し、子機の一覧を実際に合わせる。
    /// 新しいアドレスは識別を読み、OSL 子機なら加える。既知の UID が別のアドレスに現れたら
    /// アドレスが変わったとみなして移す。応答の無くなった子機は通信失敗として一覧に残す。
    /// </summary>
    private async Task RescanAsync(II2cBus bus, CancellationToken ct)
    {
      var present = (await bus.ScanAsync(OslRegister.MinAddress, OslRegister.MaxAddress, ct)).ToHashSet();
      var unknown = new List<byte>();

      foreach (var n in Nodes.Where(n => !present.Contains(n.Device.Address))) n.MarkOffline();

      foreach (var addr in present)
      {
        var node = Nodes.FirstOrDefault(n => n.Device.Address == addr);
        if (node != null && node.IsOnline) continue;

        // 新しいアドレス、または通信失敗から戻った子機: 識別を読んで確かめる (書き込みはしない)
        var ident = OslDevice.ParseIdentity(await bus.ReadAsync(addr, OslRegister.IdentLength, ct) ?? Array.Empty<byte>());
        if (ident == null)
        {
          unknown.Add(addr);
          continue;
        }
        if (node != null && node.Uid == ident.Uid)
        {
          node.Apply(await node.Device.PollAsync(ct));
          continue;
        }

        var dev = new OslDevice(bus, addr);
        try
        {
          await dev.LoadAsync(ct);
        }
        catch (OslException)
        {
          unknown.Add(addr);
          continue;
        }

        var same = Nodes.FirstOrDefault(n => n.Uid == ident.Uid) ?? node;
        if (same != null)
        {
          same.Replace(dev);
          Nodes.Remove(same);
          Insert(same);
        }
        else
        {
          Insert(new NodeViewModel(dev));
        }
      }

      UnknownText = unknown.Count == 0 ? "" : "OSL でない機器: " + string.Join(", ", unknown.Select(a => $"0x{a:X2}"));
    }

    /// <summary>アドレスの順に並べて加える</summary>
    private void Insert(NodeViewModel node)
    {
      int i = 0;
      while (i < Nodes.Count && Nodes[i].Device.Address < node.Device.Address) i++;
      Nodes.Insert(i, node);
    }

    /// <summary>子機の詳細画面でアドレスを変えた後に、並びを直す</summary>
    public void Resort(NodeViewModel node)
    {
      if (!Nodes.Remove(node)) return;
      Insert(node);
    }

    [RelayCommand]
    private Task UseDemoAsync() => _session.SetDemoAsync(true);

    /// <summary>一覧で選んだ子機。選んだら詳細画面を開き、選択を戻す</summary>
    [ObservableProperty]
    private NodeViewModel? selectedNode;

    partial void OnSelectedNodeChanged(NodeViewModel? value)
    {
      if (value == null) return;
      SelectedNode = null;
      _ = Shell.Current.GoToAsync("node", new Dictionary<string, object> { ["Node"] = value });
    }

    #endregion

    #region 記録

    [RelayCommand]
    private async Task ToggleRecordAsync()
    {
      if (IsRecording)
      {
        StopRecord();
        return;
      }
      if (_recorder.Count > 0)
      {
        bool ok = await Shell.Current.DisplayAlertAsync("記録", "前の記録を消して、新しく記録を始めますか?", "始める", "やめる");
        if (!ok) return;
      }
      _recorder.Clear();
      IsRecording = true;
      HasRecord = false;
      _background.Start();
      DeviceDisplay.Current.KeepScreenOn = true;
      _recordCts = new CancellationTokenSource();
      _ = RecordLoopAsync(_recordCts.Token);
    }

    private void StopRecord()
    {
      _recordCts?.Cancel();
      _recordCts = null;
      IsRecording = false;
      HasRecord = _recorder.Count > 0;
      _background.Stop();
      DeviceDisplay.Current.KeepScreenOn = false;
      UpdateRecordText();
    }

    private async Task RecordLoopAsync(CancellationToken ct)
    {
      using var timer = new PeriodicTimer(TimeSpan.FromSeconds(RecordIntervalSec));
      try
      {
        do
        {
          _recorder.Add(DateTime.Now, Nodes.SelectMany(n => n.RecordCells()).ToList());
          UpdateRecordText();
        }
        while (await timer.WaitForNextTickAsync(ct));
      }
      catch (OperationCanceledException) { }
    }

    private void UpdateRecordText()
    {
      var start = _recorder.StartedAt;
      if (start == null)
      {
        RecordText = "";
        return;
      }
      var el = DateTime.Now - start.Value;
      RecordText = IsRecording
        ? $"記録中 {(int)el.TotalHours}:{el:mm\\:ss}  {_recorder.Count} 行"
        : $"記録 {_recorder.Count} 行 ({start:HH:mm:ss} から)";
    }

    [RelayCommand]
    private async Task ExportAsync()
    {
      if (_recorder.Count == 0) return;
      try
      {
        string file = Path.Combine(FileSystem.CacheDirectory, $"PoEMLink_{_recorder.StartedAt:yyyyMMdd_HHmmss}.csv");
        await File.WriteAllTextAsync(file, _recorder.ToCsv(), new System.Text.UTF8Encoding(true));
        await Share.Default.RequestAsync(new ShareFileRequest { Title = "記録の書き出し", File = new ShareFile(file) });
      }
      catch (Exception ex)
      {
        await Shell.Current.DisplayAlertAsync("エラー", $"書き出しに失敗しました: {ex.Message}", "OK");
      }
    }

    #endregion
  }
}
