namespace PoEMLink.App.Services
{
  /// <summary>
  /// 記録中にアプリがバックグラウンドへ回っても計測を続けるための仕組み (E-Sensor の ILoggingService)。
  /// Android はフォアグラウンドサービス、iOS はバックグラウンドタスク。Windows では何もしない。
  /// </summary>
  public interface IBackgroundService
  {
    void Start();

    void Stop();
  }

  public sealed class NoBackgroundService : IBackgroundService
  {
    public void Start() { }

    public void Stop() { }
  }
}
