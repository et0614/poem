using Microsoft.Maui.Controls.Shapes;

namespace PoEMLink.App.Views
{
  /// <summary>
  /// アイコンのマンボウ (青い角丸の四角に白いマンボウ)。<see cref="Pulse"/> が変わるたびに
  /// 口を 1 回閉じる (パクッ)。記録の 1 行ごとに Pulse を増やすと、データが流れている間だけ口が動く。
  ///
  /// 絵は Resources/Images/mola.svg (口を開けた形) と mola_closed.svg (閉じた形)。どちらも
  /// アイコン・シルクと同じ形から hardware/logo/make_mola.py で作り、ビルド時に PNG になる。
  /// 白いマンボウの目と口は抜きなので、下地の青が透けて見える。
  /// </summary>
  public class MolaIndicator : ContentView
  {
    /// <summary>口を閉じている時間</summary>
    private static readonly TimeSpan ChompTime = TimeSpan.FromMilliseconds(160);

    private static readonly ImageSource Open = ImageSource.FromFile("mola.png");
    private static readonly ImageSource Closed = ImageSource.FromFile("mola_closed.png");

    private readonly Image _open, _closed;
    private readonly Border _badge;
    private int _chomp;

    public static readonly BindableProperty PulseProperty = BindableProperty.Create(
      nameof(Pulse), typeof(int), typeof(MolaIndicator), 0,
      propertyChanged: (b, _, _) => ((MolaIndicator)b).Chomp());

    public static readonly BindableProperty BadgeColorProperty = BindableProperty.Create(
      nameof(BadgeColor), typeof(Color), typeof(MolaIndicator), Color.FromArgb("#1565C0"),
      propertyChanged: (b, _, n) => ((MolaIndicator)b)._badge.BackgroundColor = (Color)n);

    /// <summary>変わるたびに 1 回パクッとする</summary>
    public int Pulse
    {
      get => (int)GetValue(PulseProperty);
      set => SetValue(PulseProperty, value);
    }

    /// <summary>下地の角丸の四角の色 (アイコンと同じ青)。目と口の抜きからこの色が見える</summary>
    public Color BadgeColor
    {
      get => (Color)GetValue(BadgeColorProperty);
      set => SetValue(BadgeColorProperty, value);
    }

    public MolaIndicator()
    {
      // 2 枚を重ねて置き、見せる方を切り替える (Source の差し替えより読み込みが起きない)
      _open = new Image { Source = Open, WidthRequest = 19, HeightRequest = 24 };
      _closed = new Image { Source = Closed, WidthRequest = 19, HeightRequest = 24, IsVisible = false };
      _badge = new Border
      {
        Content = new Grid { Children = { _open, _closed } },
        BackgroundColor = BadgeColor,
        StrokeThickness = 0,
        StrokeShape = new RoundRectangle { CornerRadius = 7 },
        WidthRequest = 32,
        HeightRequest = 32,
        Padding = new Thickness(7, 4, 6, 4),
      };
      Content = _badge;
    }

    private async void Chomp()
    {
      int n = ++_chomp;
      SetClosed(true);
      await Task.Delay(ChompTime);
      // 閉じている間に次の Pulse が来たら、そちらが開ける
      if (n == _chomp) SetClosed(false);
    }

    private void SetClosed(bool closed)
    {
      _closed.IsVisible = closed;
      _open.IsVisible = !closed;
    }
  }
}
