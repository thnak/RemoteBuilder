using System.Diagnostics;
using System.Text;
using H.NotifyIcon;
using H.NotifyIcon.Core;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Graphics;

namespace RemoteBuilder.Tray;

public sealed partial class MainWindow : Window
{
    private TraySettings _settings;
    private AgentClient _agent;
    private readonly UpdateChecker _updater = new();
    private readonly DispatcherQueueTimer _timer;
    private readonly Dictionary<string, string> _seen = new();
    private string _selectedJob = "";
    private long _logOffset;
    private bool _polling;
    private bool _firstPoll = true;
    private bool _isVisible;

    // The tray menu runs in H.NotifyIcon's PopupMenu mode, which renders a
    // native Win32 menu and only invokes MenuFlyoutItem.Command - Click
    // handlers on those items never fire.
    public RelayCommand ShowHideCommand { get; }
    public RelayCommand RefreshCommand { get; }
    public RelayCommand CheckUpdatesCommand { get; }
    public RelayCommand OpenReleasesCommand { get; }
    public RelayCommand ShowSettingsCommand { get; }
    public RelayCommand ExitCommand { get; }

    public MainWindow()
    {
        ShowHideCommand = new RelayCommand(ToggleWindow);
        RefreshCommand = new RelayCommand(
            () => Refresh_Click(this, new RoutedEventArgs()));
        CheckUpdatesCommand = new RelayCommand(
            () => CheckUpdates_Click(this, new RoutedEventArgs()));
        OpenReleasesCommand = new RelayCommand(
            () => OpenReleases_Click(this, new RoutedEventArgs()));
        ShowSettingsCommand = new RelayCommand(ShowSettingsFromTray);
        ExitCommand = new RelayCommand(ExitApp);

        InitializeComponent();
        SetWindowSize(780, 580);

        _settings = TraySettings.Load();
        _agent = new AgentClient(_settings);

        var iconPath = Path.Combine(
            AppContext.BaseDirectory, "Assets", "tray.ico");
        if (File.Exists(iconPath))
        {
            TrayIcon.Icon = new System.Drawing.Icon(iconPath);
        }
        TrayIcon.LeftClickCommand = new RelayCommand(ToggleWindow);

        var queue = DispatcherQueue.GetForCurrentThread();
        _timer = queue.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(_settings.PollSeconds);
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

    // ---------- polling ----------

    private async Task PollAsync()
    {
        if (_polling)
        {
            return;
        }
        _polling = true;
        try
        {
            // The agent binds 0.0.0.0, so it is reachable on every interface;
            // _agent.Host is only the address this tray uses to reach it.
            AgentText.Text = $"{_agent.Host}:{_agent.Port}";
            ToolTipService.SetToolTip(AgentText,
                $"The agent listens on all interfaces (0.0.0.0:{_agent.Port}). "
                + $"This tray connects to it via {_agent.Host}:{_agent.Port}.");

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
                StatusText.Text = string.IsNullOrWhiteSpace(inv.agentVer)
                    ? "online"
                    : $"online - agent v{inv.agentVer}";
                StatusText.Foreground =
                    new SolidColorBrush(Colors.Green);
                if (!string.IsNullOrEmpty(inv.ip))
                {
                    AgentText.Text = $"{inv.ip}:{_agent.Port}";
                }
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

    // ---------- settings ----------

    private void ShowSettings_Click(object sender, RoutedEventArgs e) =>
        ShowSettings();

    private void ShowSettings()
    {
        HostBox.Text = _settings.Host;
        PortBox.Text = _settings.Port.ToString();
        IntervalBox.Text = _settings.PollSeconds.ToString();
        TokenBox.Password = "";
        TokenHintText.Text = _settings.HasExplicitToken
            ? $"Using the token saved in settings ({_settings.MaskedToken})."
            : $"Using the token from {_settings.TokenSource}: "
              + $"{_settings.MaskedToken}. Leave blank to keep it.";
        TestResultText.Text = "";
        SaveMessageText.Text = "";
        AutostartCheck.IsChecked = StartupShortcut.IsEnabled();

        MainPanel.Visibility = Visibility.Collapsed;
        SettingsPanel.Visibility = Visibility.Visible;
        _ = RefreshServiceStatusAsync();
    }

    private void CancelSettings_Click(object sender, RoutedEventArgs e) =>
        ShowMain();

    private void ShowMain()
    {
        SettingsPanel.Visibility = Visibility.Collapsed;
        MainPanel.Visibility = Visibility.Visible;
    }

    private bool TryBuildFromForm(
        out TraySettings candidate, out string error)
    {
        candidate = new TraySettings();
        error = "";

        var host = HostBox.Text.Trim();
        if (host.Length == 0)
        {
            error = "Host is required.";
            return false;
        }
        if (TraySettings.IsWildcardHost(host))
        {
            error = $"{host} is a listen address and cannot be connected "
                + "to. Use 127.0.0.1 for this machine, or its LAN IP.";
            return false;
        }
        if (!int.TryParse(PortBox.Text.Trim(), out var port)
            || port < 1 || port > 65535)
        {
            error = "Port must be a number between 1 and 65535.";
            return false;
        }
        if (!int.TryParse(IntervalBox.Text.Trim(), out var seconds)
            || seconds < 1 || seconds > 3600)
        {
            error = "Refresh interval must be between 1 and 3600 seconds.";
            return false;
        }

        candidate.Host = host;
        candidate.Port = port;
        candidate.PollSeconds = seconds;
        candidate.StartWithWindows = AutostartCheck.IsChecked == true;
        candidate.Token = TokenBox.Password.Trim();
        candidate.Resolve();
        return true;
    }

    private async void Save_Click(object sender, RoutedEventArgs e)
    {
        if (!TryBuildFromForm(out var candidate, out var error))
        {
            SaveMessageText.Text = error;
            return;
        }

        try
        {
            StartupShortcut.SetEnabled(candidate.StartWithWindows);
        }
        catch (Exception ex)
        {
            SaveMessageText.Text = $"Startup shortcut: {ex.Message}";
        }

        _settings = candidate;
        _settings.Save();
        ApplySettings();
        await PollAsync();
        ShowMain();
    }

    private void ApplySettings()
    {
        _agent.Dispose();
        _agent = new AgentClient(_settings);
        _timer.Interval = TimeSpan.FromSeconds(_settings.PollSeconds);
    }

    private async void Test_Click(object sender, RoutedEventArgs e)
    {
        if (!TryBuildFromForm(out var candidate, out var error))
        {
            TestResultText.Text = error;
            return;
        }

        TestResultText.Text = "testing…";
        using var probe = new AgentClient(candidate);
        var ok = await Task.Run(probe.Reachable);
        TestResultText.Text = ok
            ? $"OK - {candidate.Endpoint} answered using token "
              + $"{candidate.MaskedToken}."
            : $"No response from {candidate.Endpoint} - wrong port, or the "
              + "token does not match the agent's.";
    }

    private async Task RefreshServiceStatusAsync()
    {
        var state = await Task.Run(AgentService.State);
        ServiceStatusText.Text = state == "not installed"
            ? $"{AgentService.DisplayName} service is not installed."
            : $"{AgentService.DisplayName} service is {state}.";
    }

    private async void ServiceStart_Click(
        object sender, RoutedEventArgs e) =>
        await ControlServiceAsync(AgentService.Start);

    private async void ServiceStop_Click(
        object sender, RoutedEventArgs e) =>
        await ControlServiceAsync(AgentService.Stop);

    private async void ServiceRestart_Click(
        object sender, RoutedEventArgs e) =>
        await ControlServiceAsync(AgentService.Restart);

    private async Task ControlServiceAsync(Action action)
    {
        try
        {
            action();
        }
        catch (Exception ex)
        {
            ServiceStatusText.Text = $"Could not control the service: "
                + ex.Message;
            return;
        }
        await Task.Delay(2500);
        await RefreshServiceStatusAsync();
    }

    // ---------- commands ----------

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

    private void ShowSettingsFromTray()
    {
        ShowSettings();
        if (!_isVisible)
        {
            ToggleWindow();
        }
    }

    private void ExitApp()
    {
        App.HandleClosedEvents = false;
        _timer.Stop();
        _agent.Dispose();
        TrayIcon.Dispose();
        Close();
        Application.Current.Exit();
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
