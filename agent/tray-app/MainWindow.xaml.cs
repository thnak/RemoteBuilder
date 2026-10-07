using System.Diagnostics;
using System.Text;
using H.NotifyIcon;
using H.NotifyIcon.Core;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Graphics;

namespace RemoteBuilder.Tray;

public sealed partial class MainWindow : Window
{
    private readonly AgentClient _agent = new();
    private readonly UpdateChecker _updater = new();
    private readonly DispatcherQueueTimer _timer;
    private readonly Dictionary<string, string> _seen = new();
    private string _selectedJob = "";
    private long _logOffset;
    private bool _polling;
    private bool _firstPoll = true;
    private bool _isVisible;

    public MainWindow()
    {
        InitializeComponent();
        SetWindowSize(780, 580);

        var iconPath = Path.Combine(
            AppContext.BaseDirectory, "Assets", "tray.ico");
        if (File.Exists(iconPath))
        {
            TrayIcon.Icon = new System.Drawing.Icon(iconPath);
        }
        TrayIcon.LeftClickCommand =
            new RelayCommand(ToggleWindow);

        var queue = DispatcherQueue.GetForCurrentThread();
        _timer = queue.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(2);
        _timer.Tick += (_, _) => _ = PollAsync();
        _timer.Start();
        _ = PollAsync();
    }

    private void SetWindowSize(int width, int height)
    {
        try
        {
            var hwnd = WinRT.Interop.WindowNative
                .GetWindowHandle(this);
            var id = Microsoft.UI.Win32Interop
                .GetWindowIdFromWindow(hwnd);
            var appWindow =
                Microsoft.UI.Windowing.AppWindow
                    .GetFromWindowId(id);
            appWindow?.Resize(new SizeInt32(width, height));
        }
        catch (Exception)
        {
            // window sizing is cosmetic - ignore
        }
    }

    private void ToggleWindow()
    {
        if (_isVisible)
        {
            _isVisible = false;
            this.Hide();
        }
        else
        {
            _isVisible = true;
            this.Show();
            Activate();
        }
    }

    private async Task PollAsync()
    {
        if (_polling)
        {
            return;
        }
        _polling = true;
        try
        {
            var inv = await _agent.GetInventoryAsync();
            if (inv is null)
            {
                StatusText.Text = "offline";
                StatusText.Foreground =
                    new SolidColorBrush(Colors.OrangeRed);
                TrayIcon.ToolTipText =
                    "RemoteBuilder agent - offline";
            }
            else
            {
                StatusText.Text = $"online - agent v{inv.version}";
                StatusText.Foreground =
                    new SolidColorBrush(Colors.Green);
                MachineText.Text = string.IsNullOrEmpty(inv.ip)
                    ? inv.name
                    : $"{inv.name} ({inv.ip})";
                CoresText.Text = inv.cores.ToString();
                LoadText.Text = $"{inv.cpuLoadPct:0.0}%";
                MemText.Text = inv.memTotalMB > 0
                    ? $"{(inv.memTotalMB - inv.memFreeMB) / 1024.0:0.0} / "
                      + $"{inv.memTotalMB / 1024.0:0.0} GB"
                    : "-";
                JobsText.Text =
                    $"{inv.jobs.running} running / {inv.jobs.queued} queued";
                AgentText.Text = inv.os;
                TrayIcon!.ToolTipText = MakeTooltip(inv);
            }

            var jobs = await _agent.ListJobsAsync();
            UpdateJobsList(jobs);
            DetectJobEvents(jobs);
            _firstPoll = false;
            await RefreshLogAsync();
        }
        finally
        {
            _polling = false;
        }
    }

    private static string MakeTooltip(Inventory inv)
    {
        var mem = inv.memTotalMB > 0
            ? $"mem {inv.memFreeMB / 1024.0:0}/{inv.memTotalMB / 1024.0:0}GB"
            : "mem -";
        var s = $"{inv.name} | {inv.cores} cores | "
            + $"load {inv.cpuLoadPct:0}% | {mem} | "
            + $"jobs {inv.jobs.running}/{inv.jobs.queued}";
        return s.Length <= 63 ? s : s[..63];
    }

    private void UpdateJobsList(JobSummary[] jobs)
    {
        var keep = _selectedJob;
        JobsList.Items.Clear();
        foreach (var j in jobs)
        {
            JobsList.Items.Add(new JobRow(j));
        }
        foreach (JobRow? row in JobsList.Items)
        {
            if (row?.Id == keep)
            {
                JobsList.SelectedItem = row;
                break;
            }
        }
    }

    private void DetectJobEvents(JobSummary[] jobs)
    {
        var current = jobs.ToDictionary(j => j.id, j => j.status);
        if (_firstPoll)
        {
            _seen.Clear();
            foreach (var kv in current)
            {
                _seen[kv.Key] = kv.Value;
            }
            return;
        }

        foreach (var j in jobs)
        {
            if (!_seen.TryGetValue(j.id, out var old))
            {
                if (j.status is "running" or "queued")
                {
                    Notify("RemoteBuilder",
                        $"Job {j.id} started: {j.cmd}");
                }
            }
            else if (old != j.status
                     && j.status is "done" or "killed" or "failed")
            {
                var exit = j.exitCode is int code
                    ? $" (exit {code})"
                    : "";
                Notify("RemoteBuilder",
                    $"Job {j.id} {j.status}{exit}");
            }
        }

        _seen.Clear();
        foreach (var kv in current)
        {
            _seen[kv.Key] = kv.Value;
        }
    }

    private void Notify(string title, string message) =>
        TrayIcon.ShowNotification(
            title: title,
            message: message,
            icon: NotificationIcon.Info,
            sound: true,
            respectQuietTime: true,
            realtime: false,
            timeout: null);

    private async Task RefreshLogAsync()
    {
        if (_selectedJob.Length == 0)
        {
            return;
        }
        var text = await _agent.GetLogTailAsync(_selectedJob, _logOffset);
        if (text.Length == 0)
        {
            return;
        }
        LogText.Text += text;
        _logOffset += Encoding.UTF8.GetByteCount(text);
        LogScroll.ChangeView(
            null, LogScroll.ScrollableHeight, null);
    }

    private async void Refresh_Click(object sender, RoutedEventArgs e)
    {
        RefreshButton.IsEnabled = false;
        await PollAsync();
        RefreshButton.IsEnabled = true;
    }

    private async void CheckUpdates_Click(object sender, RoutedEventArgs e)
    {
        UpdateButton.IsEnabled = false;
        try
        {
            var rel = await _updater.GetLatestAsync();
            if (rel is null)
            {
                StatusText.Text = "update check failed (offline?)";
                return;
            }
            if (rel.Version > _updater.Local)
            {
                StatusText.Text = $"update available: {rel.Tag}";
                Notify("RemoteBuilder update",
                    $"{rel.Tag} is available (this tray is v{_updater.Local}). "
                    + "Download it from the releases page.");
            }
            else
            {
                StatusText.Text = "up to date";
            }
        }
        finally
        {
            UpdateButton.IsEnabled = true;
        }
    }

    private void OpenReleases_Click(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo(UpdateChecker.ReleasesUrl)
        {
            UseShellExecute = true,
        });

    private async void Kill_Click(object sender, RoutedEventArgs e)
    {
        if (JobsList.SelectedItem is not JobRow row)
        {
            return;
        }
        KillButton.IsEnabled = false;
        await _agent.KillJobAsync(row.Id);
        await PollAsync();
        KillButton.IsEnabled = true;
    }

    private void JobsList_SelectionChanged(
        object sender, SelectionChangedEventArgs e)
    {
        if (JobsList.SelectedItem is JobRow row)
        {
            _selectedJob = row.Id;
            _logOffset = 0;
            LogText.Text = "";
            _ = RefreshLogAsync();
        }
    }

    private void ShowHide_Click(object sender, RoutedEventArgs e) =>
        ToggleWindow();

    private void Exit_Click(object sender, RoutedEventArgs e)
    {
        App.HandleClosedEvents = false;
        _timer.Stop();
        TrayIcon.Dispose();
        Close();
    }
}

public sealed class JobRow
{
    public string Id { get; }
    public string Status { get; }
    public string Exit { get; }
    public string LogSize { get; }
    public string Cmd { get; }

    public JobRow(JobSummary j)
    {
        Id = j.id;
        Status = j.status;
        Exit = j.exitCode is int c ? c.ToString() : "-";
        LogSize = j.logBytes switch
        {
            < 1024 => $"{j.logBytes} B",
            < 1024 * 1024 => $"{j.logBytes / 1024.0:0.0} KB",
            _ => $"{j.logBytes / 1024.0 / 1024.0:0.0} MB",
        };
        Cmd = j.cmd;
    }
}
