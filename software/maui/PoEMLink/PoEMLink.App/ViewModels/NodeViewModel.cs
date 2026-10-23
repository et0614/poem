using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using PoEMLink.App.Services;
using PoEMLink.Recording;
using PoEMLink.Osl;

namespace PoEMLink.App.ViewModels
{
  /// <summary>状態の分類 (表示の色分け用)</summary>
  public enum StatusKind
  {
    Ok,
    NotReady,     // 64 準備中
    OutOfRange,   // 2 上限超過、3 下限未満
    Fault,        // 1 センサ応答なし、7 異常、その他
    CommFailure,  // 12 通信失敗 (子機を読めない)
  }

  /// <summary>1 チャネルの表示</summary>
  public partial class ChannelViewModel : ObservableObject
  {
    public const byte CommFailureStatus = 12;

    public OslChannel Channel { get; }

    public string Name => Channel.Name;

    public string UnitName => Channel.UnitName;

    public bool IsAuxiliary => Channel.IsAuxiliary;

    [ObservableProperty]
    private string valueText = "—";

    [ObservableProperty]
    private byte status = 64;

    [ObservableProperty]
    private string statusText = OslTables.StatusName(64);

    [ObservableProperty]
    private StatusKind kind = StatusKind.NotReady;

    /// <summary>最後に読めた値 (通信失敗の間も残す)</summary>
    public double? LastValue { get; private set; }

    public ChannelViewModel(OslChannel channel) => Channel = channel;

    public void Update(double value, byte st)
    {
      // 準備中は値が意味を持たないので、表示しない
      if (st == 64)
      {
        ValueText = "—";
      }
      else
      {
        LastValue = value;
        ValueText = Channel.Format(value);
      }
      SetStatus(st);
    }

    public void SetStatus(byte st)
    {
      Status = st;
      StatusText = OslTables.StatusName(st);
      Kind = st switch
      {
        0 => StatusKind.Ok,
        64 => StatusKind.NotReady,
        2 or 3 => StatusKind.OutOfRange,
        CommFailureStatus => StatusKind.CommFailure,
        _ => StatusKind.Fault,
      };
    }
  }

  /// <summary>1 台の子機の表示と、読み取りの状態</summary>
  public partial class NodeViewModel : ObservableObject
  {
    /// <summary>この回数続けて読めなかったら通信失敗とする</summary>
    private const int FailureLimit = 3;

    /// <summary>POLL の周期の下限 (更新の速い子機でも、表示と記録にはこれで足りる)</summary>
    private const int MinPollMs = 250;

    public OslDevice Device { get; private set; }

    public ObservableCollection<ChannelViewModel> Channels { get; } = new();

    [ObservableProperty]
    private bool isOnline = true;

    /// <summary>直近の POLL (通信失敗の間は最後に読めたもの)</summary>
    public OslSample? LastSample { get; private set; }

    public DateTime NextDue { get; set; }

    private int _failures;

    public string Uid => Device.Identity.Uid;

    public string Title => string.IsNullOrWhiteSpace(Device.Name) ? Device.Identity.ModelName : Device.Name;

    public string Subtitle => $"0x{Device.Address:X2} · {Device.Identity.ModelName}";

    /// <summary>記録の見出しに使う名前 (名前が無ければアドレス)</summary>
    public string Label => string.IsNullOrWhiteSpace(Device.Name) ? $"0x{Device.Address:X2}" : Device.Name;

    public TimeSpan PollInterval => TimeSpan.FromMilliseconds(Math.Max(MinPollMs, (int)Device.Identity.PeriodMs));

    public NodeViewModel(OslDevice device)
    {
      Device = device;
      Rebuild();
    }

    /// <summary>同じ場所に別の個体、またはチャネル構成の違う子機が現れたとき</summary>
    public void Replace(OslDevice device)
    {
      Device = device;
      Rebuild();
      NotifyIdentityChanged();
    }

    private void Rebuild()
    {
      Channels.Clear();
      foreach (var ch in Device.Channels) Channels.Add(new ChannelViewModel(ch));
      LastSample = null;
      _failures = 0;
      IsOnline = true;
    }

    /// <summary>名前やアドレスが変わったときに表示を更新する</summary>
    public void NotifyIdentityChanged()
    {
      OnPropertyChanged(nameof(Title));
      OnPropertyChanged(nameof(Subtitle));
      OnPropertyChanged(nameof(Label));
    }

    /// <summary>POLL の結果を反映する (読めなかったら null)</summary>
    public void Apply(OslSample? sample)
    {
      if (sample == null)
      {
        if (++_failures >= FailureLimit) MarkOffline();
        return;
      }
      _failures = 0;
      IsOnline = true;
      LastSample = sample;
      for (int i = 0; i < Channels.Count && i < sample.Values.Length; i++)
        Channels[i].Update(sample.Values[i], sample.Status[i]);
    }

    public void MarkOffline()
    {
      if (!IsOnline) return;
      IsOnline = false;
      foreach (var ch in Channels) ch.SetStatus(ChannelViewModel.CommFailureStatus);
    }

    /// <summary>記録の 1 行分 (通信失敗の間は値を空欄にする)</summary>
    public IEnumerable<RecordCell> RecordCells()
    {
      for (int i = 0; i < Channels.Count; i++)
      {
        var ch = Channels[i];
        string header = $"{Label}_{ch.Name}" + (ch.UnitName.Length > 0 ? $"[{ch.UnitName}]" : "");
        bool hasValue = IsOnline && LastSample != null && ch.Status != 64;
        yield return new RecordCell($"{Uid}/{i}", header, ch.Channel.Type,
          hasValue ? LastSample!.Values[i] : null, ch.Status);
      }
    }
  }
}
