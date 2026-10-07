using System.Net;
using System.Net.Sockets;
using System.Text;

namespace RemoteBuilder.Tray.Tests;

/// <summary>
/// Minimal HTTP/1.1 server on 127.0.0.1 standing in for rbagent. Each
/// request is answered by <see cref="Handler"/> and recorded.
/// </summary>
internal sealed class FakeAgent : IDisposable
{
    private readonly TcpListener _listener =
        new(IPAddress.Loopback, 0);
    private readonly CancellationTokenSource _cts = new();

    public record Request(string Method, string Path, string? Authorization);

    public List<Request> Requests { get; } = new();

    public Func<Request, (int Status, string Body)> Handler { get; set; } =
        _ => (404, "{}");

    public int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;

    public FakeAgent()
    {
        _listener.Start();
        _ = AcceptLoopAsync();
    }

    public TraySettings Settings(string token = "test-token")
    {
        var settings = new TraySettings
        {
            Host = "127.0.0.1",
            Port = Port,
            Token = token,
        };
        settings.Resolve();
        return settings;
    }

    private async Task AcceptLoopAsync()
    {
        while (!_cts.IsCancellationRequested)
        {
            TcpClient client;
            try
            {
                client = await _listener.AcceptTcpClientAsync(_cts.Token);
            }
            catch (Exception)
            {
                return;
            }
            _ = ServeAsync(client);
        }
    }

    private async Task ServeAsync(TcpClient client)
    {
        using (client)
        await using (var stream = client.GetStream())
        {
            using var reader = new StreamReader(
                stream, Encoding.ASCII, leaveOpen: true);
            var requestLine = await reader.ReadLineAsync();
            if (string.IsNullOrEmpty(requestLine))
            {
                return;
            }
            string? auth = null;
            string? line;
            while (!string.IsNullOrEmpty(line = await reader.ReadLineAsync()))
            {
                var colon = line.IndexOf(':');
                if (colon > 0 && line[..colon].Equals(
                        "Authorization", StringComparison.OrdinalIgnoreCase))
                {
                    auth = line[(colon + 1)..].Trim();
                }
            }

            var parts = requestLine.Split(' ');
            var request = new Request(parts[0], parts[1], auth);
            lock (Requests)
            {
                Requests.Add(request);
            }

            var (status, body) = Handler(request);
            var bytes = Encoding.UTF8.GetBytes(body);
            var head = $"HTTP/1.1 {status} X\r\n"
                + "Content-Type: application/json\r\n"
                + $"Content-Length: {bytes.Length}\r\n"
                + "Connection: close\r\n\r\n";
            await stream.WriteAsync(Encoding.ASCII.GetBytes(head));
            await stream.WriteAsync(bytes);
        }
    }

    public void Dispose()
    {
        _cts.Cancel();
        _listener.Stop();
    }
}
