using Xunit;

namespace RemoteBuilder.Tray.Tests;

public class TraySettingsTests
{
    [Theory]
    [InlineData("0.0.0.0")]
    [InlineData("::")]
    [InlineData("[::]")]
    [InlineData("*")]
    [InlineData(" 0.0.0.0 ")]
    public void Wildcard_listen_addresses_are_detected(string host)
    {
        Assert.True(TraySettings.IsWildcardHost(host));
    }

    [Theory]
    [InlineData("127.0.0.1")]
    [InlineData("localhost")]
    [InlineData("192.168.1.20")]
    [InlineData("::1")]
    [InlineData("build-box")]
    public void Connectable_hosts_are_not_wildcards(string host)
    {
        Assert.False(TraySettings.IsWildcardHost(host));
    }

    [Fact]
    public void Explicit_token_wins_and_is_trimmed()
    {
        var settings = new TraySettings { Token = "  abc123  " };

        settings.Resolve();

        Assert.True(settings.HasExplicitToken);
        Assert.Equal("abc123", settings.EffectiveToken);
        Assert.Equal("the token saved in settings", settings.TokenSource);
    }

    [Fact]
    public void Endpoint_and_masked_token_are_formatted()
    {
        var settings = new TraySettings
        {
            Host = "10.0.0.5",
            Port = 7333,
            Token = "0123456789abcdef",
        };
        settings.Resolve();

        Assert.Equal("http://10.0.0.5:7333", settings.Endpoint);
        Assert.Equal("01234567…", settings.MaskedToken);
    }

    [Fact]
    public void Short_tokens_are_not_masked()
    {
        var settings = new TraySettings { Token = "short" };
        settings.Resolve();

        Assert.Equal("short", settings.MaskedToken);
    }
}
