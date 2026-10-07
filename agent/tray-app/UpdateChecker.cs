using System.Net.Http.Headers;
using System.Reflection;
using System.Text.Json;

namespace RemoteBuilder.Tray;

public sealed record ReleaseInfo(Version Version, string Tag, string Url);

public sealed class UpdateChecker
{
    private const string Repo = "thnak/RemoteBuilder";
    private const string Api =
        $"https://api.github.com/repos/{Repo}/releases/latest";
    public const string ReleasesUrl =
        $"https://github.com/{Repo}/releases";

    public Version Local { get; } =
        Assembly.GetExecutingAssembly().GetName().Version
        ?? new Version(0, 1, 0);

    public async Task<ReleaseInfo?> GetLatestAsync()
    {
        try
        {
            using var http = new HttpClient();
            http.DefaultRequestHeaders.UserAgent.ParseAdd(
                "RemoteBuilder-Tray");
            http.DefaultRequestHeaders.Accept.Add(
                new MediaTypeWithQualityHeaderValue(
                    "application/vnd.github+json"));
            using var cts = new CancellationTokenSource(
                TimeSpan.FromSeconds(15));
            var json = await http.GetStringAsync(Api, cts.Token);
            using var doc = JsonDocument.Parse(json);
            var tag = doc.RootElement.GetProperty("tag_name")
                .GetString() ?? "";
            var url = doc.RootElement.GetProperty("html_url")
                .GetString() ?? ReleasesUrl;
            var version = new Version(tag.TrimStart('v'));
            return new ReleaseInfo(version, tag, url);
        }
        catch (HttpRequestException) { return null; }
        catch (TaskCanceledException) { return null; }
        catch (JsonException) { return null; }
        catch (ArgumentException) { return null; }
    }
}
