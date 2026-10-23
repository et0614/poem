using PoEMLink.App.ViewModels;

namespace PoEMLink.App.Views
{
  public partial class MainPage : ContentPage
  {
    public MainPage(MainViewModel vm)
    {
      InitializeComponent();
      BindingContext = vm;
    }
  }
}
