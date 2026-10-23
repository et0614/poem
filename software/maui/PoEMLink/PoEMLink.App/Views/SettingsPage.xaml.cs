using PoEMLink.App.ViewModels;

namespace PoEMLink.App.Views
{
  public partial class SettingsPage : ContentPage
  {
    public SettingsPage(SettingsViewModel vm)
    {
      InitializeComponent();
      BindingContext = vm;
    }
  }
}
