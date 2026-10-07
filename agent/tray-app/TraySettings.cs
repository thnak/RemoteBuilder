using System.Text.Json;
using System.Text.Json.Serialization;

namespace RemoteBuilder.Tray;

/// <summary>
/// Tray monitor configuration. Stored next to the shared token so a
/// single folder holds everything the monitor needs. Values left empty
/// fall back to RB_AGENT_* environment variables and then to defaults.
/// </summary>
public sealed class TraySettings
{
    public static string Dir { get; } = Path.Combine(
        Environment.GetFolderPath(
            Environment.SpecialFolder.LocalApplicationData),
        "rb-agent");

    public static string FilePath { get; } =
        Path.Combine(Dir, "tray-settings.json");

    public string Host { get; set; } = "";

    public int Port { get; set; }

    /// Explicit token override. Empty means "use the shared token file".
    public string Token { get; set; } = "";

    public int PollSeconds { get; set; }

    public bool StartWithWindows { get; set; } = true;

    [JsonIgnore]
    public string EffectiveToken { get; private set; } = "";

    [JsonIgnore]
    public string TokenSource { get; private set; } = "";

    [JsonIgnore]
    public bool HasExplicitToken { get; private set; }

    public static TraySettings Load()
    {
        TraySettings settings = new();
        try
        {
            if (File.Exists(FilePath))
            {
                settings =
                    JsonSerializer.Deserialize<TraySettings>(
                        File.ReadAllText(FilePath)) ?? new TraySettings();
            }
        }
        catch (Exception)
        {
            settings = new TraySettings();
        }

        if (string.IsNullOrWhiteSpace(settings.Host))
        {
            var host = Environment.GetEnvironmentVariable("RB_AGENT_HOST");
            settings.Host = string.IsNullOrWhiteSpace(host)
                ? "127.0.0.1"
                : host;
        }

        // The agent listens on 0.0.0.0, but that address cannot be
        // connected to; a saved wildcard host would leave the tray offline.
        if (IsWildcardHost(settings.Host))
        {
            settings.Host = "127.0.0.1";
        }

        if (settings.Port <= 0)
        {
            settings.Port =
                int.TryParse(
                    Environment.GetEnvironmentVariable("RB_AGENT_PORT"),
                    out var port) && port > 0
                    ? port
                    : 7333;
        }

        if (settings.PollSeconds <= 0)
        {
            settings.PollSeconds = 2;
        }

        settings.Resolve();
        return settings;
    }

    public static bool IsWildcardHost(string host) =>
        host.Trim().Trim('[', ']') is "0.0.0.0" or "::" or "*";

    /// Resolves the token actually used and records where it came from.
    public void Resolve()
    {
        HasExplicitToken = !string.IsNullOrWhiteSpace(Token);
        if (HasExplicitToken)
        {
            EffectiveToken = Token.Trim();
            TokenSource = "the token saved in settings";
            return;
        }

        var env = Environment.GetEnvironmentVariable("RB_AGENT_TOKEN");
        if (!string.IsNullOrWhiteSpace(env))
        {
            EffectiveToken = env.Trim();
            TokenSource = "the RB_AGENT_TOKEN environment variable";
            return;
        }

        var local = ReadTokenFile(
            Environment.GetFolderPath(
                Environment.SpecialFolder.LocalApplicationData));
        if (local.Path.Length > 0)
        {
            EffectiveToken = local.Token;
            TokenSource = local.Path;
            return;
        }

        var common = ReadTokenFile(
            Environment.GetFolderPath(
                Environment.SpecialFolder.CommonApplicationData));
        if (common.Path.Length > 0)
        {
            EffectiveToken = common.Token;
            TokenSource = common.Path;
            return;
        }

        EffectiveToken = "";
        TokenSource = "(no token file found)";
    }

    private static (string Token, string Path) ReadTokenFile(string baseDir)
    {
        if (string.IsNullOrEmpty(baseDir))
        {
            return ("", "");
        }
        var file = Path.Combine(baseDir, "rb-agent", "token.txt");
        try
        {
            return File.Exists(file)
                ? (File.ReadAllText(file).Trim(), file)
                : ("", "");
        }
        catch (Exception)
        {
            return ("", "");
        }
    }

    public void Save()
    {
        Directory.CreateDirectory(Dir);
        File.WriteAllText(
            FilePath,
            JsonSerializer.Serialize(
                this,
                new JsonSerializerOptions { WriteIndented = true }));
    }

    public string Endpoint => $"http://{Host}:{Port}";

    public string MaskedToken => EffectiveToken.Length <= 8
        ? EffectiveToken
        : $"{EffectiveToken[..8]}…";
}
