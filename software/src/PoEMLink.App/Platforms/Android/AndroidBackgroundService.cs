using Android.App;
using Android.Content;
using Android.OS;
using AndroidX.Core.App;
using PoEMLink.App.Services;

namespace PoEMLink.App.Platforms.Android
{
  public class AndroidBackgroundService : IBackgroundService
  {
    public void Start()
    {
      Platform.AppContext.StartForegroundService(new Intent(Platform.AppContext, typeof(RecordingForegroundService)));
    }

    public void Stop()
    {
      Platform.AppContext.StopService(new Intent(Platform.AppContext, typeof(RecordingForegroundService)));
    }
  }

  /// <summary>記録中であることを通知に出し、アプリが裏に回っても止められないようにする</summary>
  [Service(ForegroundServiceType = global::Android.Content.PM.ForegroundService.TypeDataSync)]
  public class RecordingForegroundService : Service
  {
    private const string ChannelId = "recording_channel";
    private const int NotificationId = 1001;

    public override IBinder? OnBind(Intent? intent) => null;

    public override StartCommandResult OnStartCommand(Intent? intent, StartCommandFlags flags, int startId)
    {
      var channel = new NotificationChannel(ChannelId, "記録", NotificationImportance.Low);
      ((NotificationManager)GetSystemService(NotificationService)!).CreateNotificationChannel(channel);

      var open = new Intent(this, typeof(MainActivity));
      open.SetFlags(ActivityFlags.ClearTop | ActivityFlags.SingleTop);
      var pending = PendingIntent.GetActivity(this, 0, open, PendingIntentFlags.Immutable | PendingIntentFlags.UpdateCurrent);

      var builder = new NotificationCompat.Builder(this, ChannelId);
      builder.SetContentTitle("PoEM Link 記録中");
      builder.SetContentText("子機の計測値を記録しています");
      builder.SetSmallIcon(global::Android.Resource.Drawable.StatNotifySync);
      builder.SetContentIntent(pending);
      builder.SetOngoing(true);
      var notification = builder.Build()!;

      if (OperatingSystem.IsAndroidVersionAtLeast(29))
        StartForeground(NotificationId, notification, global::Android.Content.PM.ForegroundService.TypeDataSync);
      else
        StartForeground(NotificationId, notification);

      return StartCommandResult.Sticky;
    }
  }
}
