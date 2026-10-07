using System.Diagnostics;
using System.Text;
using System.Text.RegularExpressions;

namespace RemoteBuilder.Tray;

/// <summary>
/// Reads and controls the agent's Windows service. Reading the state
/// needs no elevation; starting and stopping it does, so those run in an
/// elevated helper after the usual UAC prompt.
/// </summary>
public static partial class AgentService
{
    public const string Name = "RemoteBuilderAgent";
    public const string DisplayName = "RemoteBuilder Agent";

    [GeneratedRegex(@"STATE\s*:\s*\d+\s+(\w+)")]
    private static partial Regex StatePattern();

    public static string State()
    {
        try
        {
            var info = new ProcessStartInfo("sc.exe", $"query {Name}")
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            using var process = Process.Start(info);
            if (process is null)
            {
                return "unknown";
            }
            var output = process.StandardOutput.ReadToEnd()
                + process.StandardError.ReadToEnd();
            process.WaitForExit(4000);
            var match = StatePattern().Match(output);
            return match.Success ? match.Groups[1].Value : "not installed";
        }
        catch (Exception)
        {
            return "unknown";
        }
    }

    public static string IsInstalled()
    {
        var state = State();
        return state == "not installed" ? "not installed" : state;
    }

    public static void Start() =>
        RunElevated($"Start-Service {Name}");

    public static void Stop() =>
        RunElevated($"Stop-Service {Name} -Force");

    /// Stop then start: more reliable than Restart-Service when the
    /// service is mid-transition.
    public static void Restart() => RunElevated(
        $"Stop-Service {Name} -Force -ErrorAction SilentlyContinue; " +
        $"Start-Service {Name}");

    private static void RunElevated(string command)
    {
        var encoded = Convert.ToBase64String(
            Encoding.Unicode.GetBytes(command));
        Process.Start(new ProcessStartInfo("powershell.exe")
        {
            Arguments = $"-NoProfile -NonInteractive -EncodedCommand {encoded}",
            Verb = "runas",
            UseShellExecute = true,
            WindowStyle = ProcessWindowStyle.Hidden,
        });
    }
}
