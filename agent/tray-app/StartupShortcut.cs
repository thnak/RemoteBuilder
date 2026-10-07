using System.Reflection;

namespace RemoteBuilder.Tray;

/// <summary>
/// Manages the per-user Startup shortcut that launches the tray at sign-in.
/// </summary>
public static class StartupShortcut
{
    public static string FilePath { get; } = Path.Combine(
        Environment.GetFolderPath(
            Environment.SpecialFolder.ApplicationData),
        "Microsoft", "Windows", "Start Menu", "Programs", "Startup",
        "RemoteBuilder Tray.lnk");

    public static bool IsEnabled() => File.Exists(FilePath);

    public static void SetEnabled(bool enabled)
    {
        if (!enabled)
        {
            if (File.Exists(FilePath))
            {
                File.Delete(FilePath);
            }
            return;
        }

        var target = Environment.ProcessPath
            ?? throw new InvalidOperationException(
                "cannot locate the tray executable");

        var type = Type.GetTypeFromProgID("WScript.Shell")
            ?? throw new InvalidOperationException(
                "WScript.Shell is unavailable on this machine");
        var shell = Activator.CreateInstance(type)!;
        var link = type.InvokeMember(
            "CreateShortcut", BindingFlags.InvokeMethod, null, shell,
            new object[] { FilePath });
        var linkType = link!.GetType();
        linkType.InvokeMember(
            "TargetPath", BindingFlags.SetProperty, null, link,
            new object[] { target });
        linkType.InvokeMember(
            "WorkingDirectory", BindingFlags.SetProperty, null, link,
            new object[] { AppContext.BaseDirectory });
        linkType.InvokeMember(
            "Description", BindingFlags.SetProperty, null, link,
            new object[] { "RemoteBuilder agent tray monitor" });
        linkType.InvokeMember("Save", BindingFlags.InvokeMethod, null, link,
            null);
    }
}
