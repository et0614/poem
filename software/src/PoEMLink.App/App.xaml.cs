using PoEMLink.App.Services;

namespace PoEMLink.App
{
  public partial class App : Application
  {
    private readonly LinkSession _session;

    public App(LinkSession session)
    {
      InitializeComponent();
      _session = session;
    }

    protected override Window CreateWindow(IActivationState? activationState)
    {
      // Windows でもスマホに近い縦長の大きさで開く (画面はスマホを優先して作っている)
      var window = new Window(new AppShell()) { Title = "PoEM Link" };
#if WINDOWS
      window.Width = 480;
      window.Height = 900;
#endif
      return window;
    }

    protected override async void OnStart()
    {
      base.OnStart();
      await _session.StartAsync();
#if DEBUG
      // 開発用: 環境変数 POEMLINK_DEMO=1 なら模擬モードで起動する (実機なしで画面を確かめる)
      if (!_session.IsReady && Environment.GetEnvironmentVariable("POEMLINK_DEMO") == "1")
        await _session.SetDemoAsync(true);
#endif
    }
  }
}
