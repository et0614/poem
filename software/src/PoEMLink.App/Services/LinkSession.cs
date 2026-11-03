using PoEMLink.Link;
using PoEMLink.Midi;
using PoEMLink.Osl;
using PoEMLink.Simulation;

namespace PoEMLink.App.Services
{
  /// <summary>
  /// いま使っている PoEM Link (実機か模擬) と、その上のクライアント・バスをまとめて持つ。
  ///
  /// 実機がつながればそれを使い、つながっていなければ利用者の指定で模擬に切り替えられる。
  /// 実機が挿されたら、模擬を使っていても実機に切り替える。
  /// 状態が変わったら <see cref="StateChanged"/> を UI のスレッドで知らせる。
  /// </summary>
  public sealed class LinkSession
  {
    private readonly ILinkTransport? _real;
    private SimLinkTransport? _demo;
    private int _openId;

    public PoemLinkClient? Client { get; private set; }

    public LinkInfo? Info { get; private set; }

    public II2cBus? Bus { get; private set; }

    public ILinkTransport? Active => Client?.Transport;

    public bool IsReady => Bus != null;

    public bool IsDemo => Client != null && Client.Transport == _demo;

    /// <summary>模擬を使っているときの模擬の Link (子機の抜き差しの再現に使う)</summary>
    public SimLinkTransport? Demo => _demo;

    /// <summary>最後に起きた接続の失敗 (GET_INFO に応答が無いなど)</summary>
    public string? LastError { get; private set; }

    public event Action? StateChanged;

    public LinkSession(ILinkTransport? real)
    {
      _real = real;
      if (_real != null)
        _real.ConnectionChanged += connected => MainThread.BeginInvokeOnMainThread(() => OnRealConnectionChanged(connected));
    }

    /// <summary>起動時に、すでにつながっている実機を開く</summary>
    public async Task StartAsync()
    {
      if (_real?.IsConnected == true) await OpenAsync(_real);
    }

    private async void OnRealConnectionChanged(bool connected)
    {
      if (connected)
      {
        StopDemo();
        await OpenAsync(_real!);
      }
      else if (Client?.Transport == _real)
      {
        Close();
      }
    }

    /// <summary>模擬を使う / やめる。やめたとき実機がつながっていれば実機を開く</summary>
    public async Task SetDemoAsync(bool on)
    {
      if (on)
      {
        if (IsDemo) return;
        Close();
        _demo = SimPresets.CreateDemo();
        _demo.Connect();
        await OpenAsync(_demo);
      }
      else
      {
        StopDemo();
        if (_real?.IsConnected == true) await OpenAsync(_real);
      }
    }

    private void StopDemo()
    {
      if (_demo == null) return;
      if (IsDemo) Close();
      _demo.Disconnect();
      _demo = null;
    }

    private async Task OpenAsync(ILinkTransport transport)
    {
      Close();
      int id = ++_openId;
      var client = new PoemLinkClient(transport);
      try
      {
        var info = await client.GetInfoAsync();
        if (id != _openId) return;  // 待っている間に別の接続に切り替わった
        Client = client;
        Info = info;
        Bus = new LinkI2cBus(client, info);
        LastError = null;
      }
      catch (LinkException ex)
      {
        client.Dispose();
        LastError = $"PoEM Link が応答しません ({ex.Message})";
      }
      StateChanged?.Invoke();
    }

    private void Close()
    {
      if (Client == null) return;
      Client.Dispose();
      Client = null;
      Info = null;
      Bus = null;
      StateChanged?.Invoke();
    }
  }
}
