using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PoEMLink.App.Services;
using PoEMLink.Link;
using PoEMLink.Osl;

namespace PoEMLink.App.ViewModels
{
  /// <summary>表の 1 行 (項目名と値)</summary>
  public sealed record InfoRow(string Label, string Value);

  /// <summary>子機の詳細: 識別・拡張ステータスの表示と、名前・アドレスの変更、識別点滅</summary>
  public partial class NodeDetailViewModel : ObservableObject, IQueryAttributable
  {
    private readonly LinkSession _session;
    private readonly MainViewModel _main;

    [ObservableProperty]
    private NodeViewModel? node;

    [ObservableProperty]
    private IReadOnlyList<InfoRow> identityRows = Array.Empty<InfoRow>();

    [ObservableProperty]
    private IReadOnlyList<InfoRow> statusRows = Array.Empty<InfoRow>();

    [ObservableProperty]
    private bool canIdentify;

    [ObservableProperty]
    private bool isBusy;

    public NodeDetailViewModel(LinkSession session, MainViewModel main)
    {
      _session = session;
      _main = main;
    }

    public void ApplyQueryAttributes(IDictionary<string, object> query)
    {
      Node = query["Node"] as NodeViewModel;
      Refresh();
    }

    private void Refresh()
    {
      if (Node == null) return;
      var d = Node.Device;
      var id = d.Identity;
      var ext = d.ExtIdentity;
      var rows = new List<InfoRow>
      {
        new("名前", string.IsNullOrEmpty(d.Name) ? "(未設定)" : d.Name),
        new("I²C アドレス", $"0x{d.Address:X2} ({d.Address})"),
        new("機種", $"{id.ModelName} (0x{id.Model:X4})"),
        new("製造元", id.VendorName),
        new("ファームウェア", id.Firmware),
        new("更新周期", $"{id.PeriodMs} ms"),
        new("UID", id.Uid),
        new("チャネル", string.Join(", ", d.Channels.Select(c => c.UnitName.Length > 0 ? $"{c.Name} [{c.UnitName}]" : c.Name))),
      };
      if (ext != null)
      {
        rows.Add(new("ハードウェア版", ext.HwRev.ToString()));
        rows.Add(new("平均化の時間", ext.AvgWindowMs == 0 ? "—" : $"{ext.AvgWindowMs} ms"));
        rows.Add(new("準備時間", $"{ext.WarmupMs} ms"));
      }
      IdentityRows = rows;
      CanIdentify = d.CanIdentify;
    }

    /// <summary>拡張ステータスを読む (画面を開いたときと、更新ボタン)</summary>
    [RelayCommand]
    private async Task LoadStatusAsync()
    {
      if (Node == null) return;
      if (!_session.IsReady || !Node.IsOnline)
      {
        StatusRows = new[] { new InfoRow("状態", "通信できません") };
        return;
      }
      try
      {
        var st = await Node.Device.ReadExtStatusAsync();
        StatusRows = st == null
          ? new[] { new InfoRow("拡張ステータス", "この子機は対応していません") }
          : new[]
          {
            new InfoRow("稼働時間", FormatUptime(st.UptimeSec)),
            new InfoRow("起動要因", st.ResetCauseName),
            new InfoRow("I²C の異常回数", st.ErrI2c.ToString()),
            new InfoRow("センサの異常回数", st.ErrSensor.ToString()),
          };
      }
      catch (Exception ex) when (ex is LinkException or OslException)
      {
        StatusRows = new[] { new InfoRow("拡張ステータス", ex.Message) };
      }
    }

    private static string FormatUptime(uint sec)
    {
      var t = TimeSpan.FromSeconds(sec);
      return t.TotalDays >= 1 ? $"{(int)t.TotalDays} 日 {t:hh\\:mm\\:ss}" : t.ToString(@"hh\:mm\:ss");
    }

    [RelayCommand]
    private async Task RenameAsync()
    {
      if (Node == null) return;
      var text = await Shell.Current.DisplayPromptAsync("名前の変更", "子機の名前 (UTF-8 で 63 バイトまで)",
        "変更", "やめる", initialValue: Node.Device.Name, maxLength: 63);
      if (text == null || text == Node.Device.Name) return;
      if (System.Text.Encoding.UTF8.GetByteCount(text) > 63)
      {
        await Shell.Current.DisplayAlertAsync("名前の変更", "名前が長すぎます (UTF-8 で 63 バイトまで)", "OK");
        return;
      }
      await RunAsync(async () =>
      {
        bool ok = await Node.Device.SetNameAsync(text);
        Node.NotifyIdentityChanged();
        Refresh();
        if (!ok) await Shell.Current.DisplayAlertAsync("名前の変更", "書き込めませんでした", "OK");
      });
    }

    [RelayCommand]
    private async Task ChangeAddressAsync()
    {
      if (Node == null || _session.Bus == null) return;
      var text = await Shell.Current.DisplayPromptAsync("I²C アドレスの変更",
        "新しいアドレス (0x08〜0x77)。16 進は 0x を付ける。\n親機に付けている子機は、親機側の登録も変わります。",
        "変更", "やめる", initialValue: $"0x{Node.Device.Address:X2}", keyboard: Keyboard.Text);
      if (text == null) return;

      if (!TryParseAddress(text.Trim(), out byte addr) || addr is < OslRegister.MinAddress or > OslRegister.MaxAddress)
      {
        await Shell.Current.DisplayAlertAsync("I²C アドレスの変更", "0x08〜0x77 の値を入れてください", "OK");
        return;
      }
      if (addr == Node.Device.Address) return;

      await RunAsync(async () =>
      {
        var used = await _session.Bus.ScanAsync(addr, addr);
        if (used.Count > 0)
        {
          await Shell.Current.DisplayAlertAsync("I²C アドレスの変更", $"0x{addr:X2} はほかの機器が使っています", "OK");
          return;
        }
        bool ok = await Node.Device.SetAddressAsync(addr);
        Node.NotifyIdentityChanged();
        _main.Resort(Node);
        Refresh();
        if (!ok) await Shell.Current.DisplayAlertAsync("I²C アドレスの変更", "変更を確認できませんでした", "OK");
      });
    }

    private static bool TryParseAddress(string s, out byte addr)
    {
      if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        return byte.TryParse(s[2..], System.Globalization.NumberStyles.HexNumber, null, out addr);
      return byte.TryParse(s, out addr);
    }

    [RelayCommand]
    private Task IdentifyAsync() => RunAsync(async () =>
    {
      if (Node != null) await Node.Device.IdentifyAsync(5);
    });

    private async Task RunAsync(Func<Task> action)
    {
      if (IsBusy) return;
      IsBusy = true;
      try
      {
        await action();
      }
      catch (Exception ex) when (ex is LinkException or OslException or ArgumentException)
      {
        await Shell.Current.DisplayAlertAsync("エラー", ex.Message, "OK");
      }
      finally
      {
        IsBusy = false;
      }
    }
  }
}
