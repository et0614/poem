using PoEMLink.App.Views;

namespace PoEMLink.App
{
  public partial class AppShell : Shell
  {
    public AppShell()
    {
      InitializeComponent();
      Routing.RegisterRoute("node", typeof(NodeDetailPage));
    }
  }
}
