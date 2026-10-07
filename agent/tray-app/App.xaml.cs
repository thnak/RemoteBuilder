using H.NotifyIcon;
using Microsoft.UI.Xaml;

namespace RemoteBuilder.Tray;

public sealed partial class App : Application
{
    public static MainWindow? MainWindow { get; private set; }
    public static bool HandleClosedEvents { get; set; } = true;

    public App() => InitializeComponent();

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        MainWindow = new MainWindow();
        MainWindow.Closed += (_, e) =>
        {
            if (HandleClosedEvents)
            {
                e.Handled = true;
                MainWindow?.Hide();
            }
        };
        MainWindow.Activate();
        MainWindow.Hide();
    }
}
