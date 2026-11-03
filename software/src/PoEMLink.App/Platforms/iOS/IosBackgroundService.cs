using PoEMLink.App.Services;
using UIKit;

namespace PoEMLink.App.Platforms.iOS
{
  /// <summary>
  /// 記録中のバックグラウンド動作。音声セッション (IosLinkTransport で有効にする) と
  /// バックグラウンドタスクを組み合わせる (E-Sensor と同じ)。
  /// </summary>
  public class IosBackgroundService : IBackgroundService
  {
    private nint _taskId = -1;

    public void Start()
    {
      Stop();
      _taskId = UIApplication.SharedApplication.BeginBackgroundTask("PoEMLinkRecording", Stop);
    }

    public void Stop()
    {
      if (_taskId == -1) return;
      UIApplication.SharedApplication.EndBackgroundTask(_taskId);
      _taskId = -1;
    }
  }
}
