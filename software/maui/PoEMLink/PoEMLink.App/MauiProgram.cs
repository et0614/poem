using Microsoft.Extensions.Logging;
using PoEMLink.App.Services;
using PoEMLink.Recording;
using PoEMLink.App.ViewModels;
using PoEMLink.App.Views;
using PoEMLink.Midi;

namespace PoEMLink.App
{
  public static class MauiProgram
  {
    public static MauiApp CreateMauiApp()
    {
      var builder = MauiApp.CreateBuilder();
      builder
        .UseMauiApp<App>()
        .ConfigureFonts(fonts =>
        {
          fonts.AddFont("OpenSans-Regular.ttf", "OpenSansRegular");
          fonts.AddFont("OpenSans-Semibold.ttf", "OpenSansSemibold");
        });

      // OS ごとの MIDI (実機の PoEM Link) と、記録中のバックグラウンド動作
#if WINDOWS
      builder.Services.AddSingleton<ILinkTransport, Platforms.Windows.WindowsLinkTransport>();
      builder.Services.AddSingleton<IBackgroundService, NoBackgroundService>();
#elif ANDROID
      builder.Services.AddSingleton<ILinkTransport, Platforms.Android.AndroidLinkTransport>();
      builder.Services.AddSingleton<IBackgroundService, Platforms.Android.AndroidBackgroundService>();
#elif IOS
      builder.Services.AddSingleton<ILinkTransport, Platforms.iOS.IosLinkTransport>();
      builder.Services.AddSingleton<IBackgroundService, Platforms.iOS.IosBackgroundService>();
#endif
      builder.Services.AddSingleton(sp => new LinkSession(sp.GetService<ILinkTransport>()));
      builder.Services.AddSingleton<Recorder>();

      builder.Services.AddSingleton<MainViewModel>();
      builder.Services.AddSingleton<SettingsViewModel>();
      builder.Services.AddTransient<NodeDetailViewModel>();
      builder.Services.AddTransient<MainPage>();
      builder.Services.AddTransient<SettingsPage>();
      builder.Services.AddTransient<NodeDetailPage>();

#if DEBUG
      builder.Logging.AddDebug();
#endif

      return builder.Build();
    }
  }
}
