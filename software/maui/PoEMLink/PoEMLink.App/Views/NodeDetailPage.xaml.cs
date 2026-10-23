using PoEMLink.App.ViewModels;

namespace PoEMLink.App.Views
{
  public partial class NodeDetailPage : ContentPage
  {
    private readonly NodeDetailViewModel _vm;

    public NodeDetailPage(NodeDetailViewModel vm)
    {
      InitializeComponent();
      BindingContext = _vm = vm;
    }

    protected override void OnAppearing()
    {
      base.OnAppearing();
      _vm.LoadStatusCommand.Execute(null);
    }
  }
}
