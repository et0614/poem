using System.Globalization;
using PoEMLink.App.ViewModels;

namespace PoEMLink.App.Views
{
  /// <summary>状態の分類 → 文字の色。正常は通常の文字色、それ以外は色で目立たせる</summary>
  public class StatusKindToColorConverter : IValueConverter
  {
    private static bool IsDark => Application.Current?.RequestedTheme == AppTheme.Dark;

    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
      return (value as StatusKind? ?? StatusKind.Ok) switch
      {
        StatusKind.NotReady => Color.FromArgb(IsDark ? "#ACACAC" : "#6E6E6E"),
        StatusKind.OutOfRange => Color.FromArgb(IsDark ? "#F783AC" : "#C2255C"),
        StatusKind.Fault or StatusKind.CommFailure => Color.FromArgb(IsDark ? "#FF8787" : "#E03131"),
        _ => IsDark ? Colors.White : Color.FromArgb("#212121"),
      };
    }

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) => throw new NotSupportedException();
  }

  /// <summary>状態が正常でないときだけ状態の文字を出す</summary>
  public class IsNotOkConverter : IValueConverter
  {
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
      => value is StatusKind k && k != StatusKind.Ok;

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) => throw new NotSupportedException();
  }

  public class InverseBoolConverter : IValueConverter
  {
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) => value is not true;

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) => value is not true;
  }

  /// <summary>文字列が空でなければ true (エラーの表示など)</summary>
  public class HasTextConverter : IValueConverter
  {
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) => !string.IsNullOrEmpty(value as string);

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) => throw new NotSupportedException();
  }
}
