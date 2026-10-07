using System.Runtime.CompilerServices;
using System.Text;
using Xunit;

namespace RemoteBuilder.Tray.Tests;

internal static class ProxyEnvironment
{
    // Point every HTTP proxy variable at a dead port before any HttpClient
    // exists. If AgentClient honoured them, every test below would fail -
    // which is exactly how the tray once showed a running agent as offline.
    [ModuleInitializer]
    internal static void PoisonProxyVariables()
    {
        foreach (var name in new[]
                 { "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
                   "http_proxy", "https_proxy", "all_proxy" })
        {
            Environment.SetEnvironmentVariable(name, "http://127.0.0.1:9");
        }
        Environment.SetEnvironmentVariable("NO_PROXY", null);
        Environment.SetEnvironmentVariable("no_proxy", null);
    }
}

public class AgentClientTests : IDisposable
{
    private readonly FakeAgent _agent = new();

    public void Dispose() => _agent.Dispose();

    private const string InventoryJson = """
        {"name":"THNAK","agentVer":"0.1.0","os":"windows","arch":"x64",
         "cores":12,"cpuLoadPct":17,"memTotalMB":40347,"memFreeMB":12707,
         "ip":"192.168.1.20","jobs":{"running":1,"queued":2},
         "workspaces":[{"ws":"erploader","countFiles":7,"bytes":17241}]}
        """;

    [Fact]
    public async Task Inventory_is_parsed_from_the_agent_response()
    {
        _agent.Handler = _ => (200, InventoryJson);
        using var client = new AgentClient(_agent.Settings());

        var inv = await client.GetInventoryAsync();

        Assert.NotNull(inv);
        Assert.Equal("THNAK", inv.name);
        Assert.Equal("0.1.0", inv.agentVer);
        Assert.Equal(12, inv.cores);
        Assert.Equal(17, inv.cpuLoadPct);
        Assert.Equal(40347, inv.memTotalMB);
        Assert.Equal(12707, inv.memFreeMB);
        Assert.Equal("192.168.1.20", inv.ip);
        Assert.Equal(1, inv.jobs.running);
        Assert.Equal(2, inv.jobs.queued);
    }

    [Fact]
    public async Task Requests_carry_the_bearer_token()
    {
        _agent.Handler = _ => (200, InventoryJson);
        using var client = new AgentClient(_agent.Settings("s3cret"));

        await client.GetInventoryAsync();

        var request = Assert.Single(_agent.Requests);
        Assert.Equal("GET", request.Method);
        Assert.Equal("/inventory", request.Path);
        Assert.Equal("Bearer s3cret", request.Authorization);
    }

    [Fact]
    public async Task Proxy_environment_variables_are_ignored()
    {
        Assert.Equal(
            "http://127.0.0.1:9",
            Environment.GetEnvironmentVariable("HTTP_PROXY"));
        _agent.Handler = _ => (200, InventoryJson);
        using var client = new AgentClient(_agent.Settings());

        Assert.NotNull(await client.GetInventoryAsync());
        Assert.True(client.Reachable());
    }

    [Fact]
    public async Task Rejected_token_reads_as_offline()
    {
        _agent.Handler = _ => (401, """{"error":"unauthorized"}""");
        using var client = new AgentClient(_agent.Settings());

        Assert.Null(await client.GetInventoryAsync());
        Assert.False(client.Reachable());
    }

    [Fact]
    public async Task Unreachable_agent_reads_as_offline()
    {
        var settings = _agent.Settings();
        _agent.Dispose();
        using var client = new AgentClient(settings);

        Assert.Null(await client.GetInventoryAsync());
        Assert.Empty(await client.ListJobsAsync());
    }

    [Fact]
    public async Task Jobs_are_listed_with_optional_fields()
    {
        _agent.Handler = _ => (200, """
            {"jobs":[
              {"id":"j1","status":"running","cmd":"dotnet build",
               "exitCode":null,"logBytes":42,"startedAt":"2026-10-07T09:00:00Z"},
              {"id":"j2","status":"done","exitCode":3}
            ]}
            """);
        using var client = new AgentClient(_agent.Settings());

        var jobs = await client.ListJobsAsync();

        Assert.Equal(2, jobs.Length);
        Assert.Equal("j1", jobs[0].id);
        Assert.Equal("dotnet build", jobs[0].cmd);
        Assert.Null(jobs[0].exitCode);
        Assert.Equal(42, jobs[0].logBytes);
        Assert.Equal("j2", jobs[1].id);
        Assert.Equal("", jobs[1].cmd);
        Assert.Equal(3, jobs[1].exitCode);
        Assert.Equal(0, jobs[1].logBytes);
    }

    [Fact]
    public async Task Log_tail_is_decoded_from_base64()
    {
        var text = "Build succeeded.\r\n";
        _agent.Handler = r => (200,
            $$"""{"data":"{{Convert.ToBase64String(
                Encoding.UTF8.GetBytes(text))}}"}""");
        using var client = new AgentClient(_agent.Settings());

        var tail = await client.GetLogTailAsync("j1", 128);

        Assert.Equal(text, tail);
        Assert.Equal("/jobs/j1/log?offset=128", _agent.Requests[0].Path);
    }

    [Fact]
    public async Task Kill_posts_to_the_job()
    {
        _agent.Handler = _ => (200, "{}");
        using var client = new AgentClient(_agent.Settings());

        Assert.True(await client.KillJobAsync("j7"));

        var request = Assert.Single(_agent.Requests);
        Assert.Equal("POST", request.Method);
        Assert.Equal("/jobs/j7/kill", request.Path);
    }
}
