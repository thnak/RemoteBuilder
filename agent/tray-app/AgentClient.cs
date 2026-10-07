using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json;

namespace RemoteBuilder.Tray;

public sealed class AgentClient : IDisposable
{
    private readonly HttpClient _http = new();

    public string Host { get; }
    public int Port { get; }
    public string Token { get; }

    public AgentClient(TraySettings settings)
    {
        Host = settings.Host;
        Port = settings.Port;
        Token = settings.EffectiveToken;
        _http.Timeout = TimeSpan.FromSeconds(8);
        _http.DefaultRequestHeaders.Authorization =
            new AuthenticationHeaderValue("Bearer", Token);
    }

    public void Dispose() => _http.Dispose();

    private string Base => $"http://{Host}:{Port}";

    public async Task<Inventory?> GetInventoryAsync()
    {
        try
        {
            return await _http.GetFromJsonAsync<Inventory>(
                $"{Base}/inventory");
        }
        catch (HttpRequestException) { return null; }
        catch (TaskCanceledException) { return null; }
        catch (JsonException) { return null; }
    }

    public async Task<JobSummary[]> ListJobsAsync()
    {
        try
        {
            using var resp = await _http.GetAsync($"{Base}/jobs");
            resp.EnsureSuccessStatusCode();
            await using var stream =
                await resp.Content.ReadAsStreamAsync();
            using var doc = await JsonDocument.ParseAsync(stream);
            return doc.RootElement
                .GetProperty("jobs")
                .EnumerateArray()
                .Select(e => new JobSummary
                {
                    id = e.GetProperty("id").GetString() ?? "",
                    status = e.GetProperty("status").GetString() ?? "",
                    cmd = e.TryGetProperty("cmd", out var c)
                        ? c.GetString() ?? ""
                        : "",
                    exitCode = e.TryGetProperty("exitCode", out var x)
                        && x.ValueKind == JsonValueKind.Number
                        ? x.GetInt32()
                        : null,
                    logBytes = e.TryGetProperty("logBytes", out var l)
                        ? l.GetInt64()
                        : 0,
                    startedAt = e.TryGetProperty("startedAt", out var s)
                        ? s.GetString() ?? ""
                        : "",
                })
                .ToArray();
        }
        catch (HttpRequestException) { return Array.Empty<JobSummary>(); }
        catch (TaskCanceledException) { return Array.Empty<JobSummary>(); }
        catch (JsonException) { return Array.Empty<JobSummary>(); }
    }

    public async Task<string> GetLogTailAsync(string jobId, long offset)
    {
        try
        {
            using var resp = await _http.GetAsync(
                $"{Base}/jobs/{jobId}/log?offset={offset}");
            resp.EnsureSuccessStatusCode();
            await using var stream =
                await resp.Content.ReadAsStreamAsync();
            using var doc = await JsonDocument.ParseAsync(stream);
            var root = doc.RootElement;
            var b64 = root.GetProperty("data").GetString() ?? "";
            return System.Text.Encoding.UTF8.GetString(
                Convert.FromBase64String(b64));
        }
        catch (HttpRequestException) { return ""; }
        catch (TaskCanceledException) { return ""; }
        catch (JsonException) { return ""; }
        catch (FormatException) { return ""; }
    }

    public async Task<bool> KillJobAsync(string jobId)
    {
        try
        {
            using var resp = await _http.PostAsync(
                $"{Base}/jobs/{jobId}/kill", null);
            return resp.IsSuccessStatusCode;
        }
        catch (HttpRequestException) { return false; }
        catch (TaskCanceledException) { return false; }
    }

    public bool Reachable()
    {
        try
        {
            using var cts = new CancellationTokenSource(
                TimeSpan.FromSeconds(3));
            using var resp = _http.GetAsync(
                $"{Base}/inventory", cts.Token).GetAwaiter()
                .GetResult();
            return resp.IsSuccessStatusCode;
        }
        catch { return false; }
    }
}
