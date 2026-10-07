namespace RemoteBuilder.Tray;

public sealed class Inventory
{
    public string name { get; set; } = "";
    public int cores { get; set; }
    public double cpuLoadPct { get; set; }
    public long memFreeMB { get; set; }
    public long memTotalMB { get; set; }
    public JobCounts jobs { get; set; } = new();
    public string os { get; set; } = "";
    public string ip { get; set; } = "";
    public string version { get; set; } = "";
}

public sealed class JobCounts
{
    public int running { get; set; }
    public int queued { get; set; }
}

public sealed class JobSummary
{
    public string id { get; set; } = "";
    public string status { get; set; } = "";
    public string cmd { get; set; } = "";
    public int? exitCode { get; set; }
    public long logBytes { get; set; }
    public string startedAt { get; set; } = "";
}

public sealed class JobStatus
{
    public string status { get; set; } = "";
    public int? exitCode { get; set; }
    public long logBytes { get; set; }
}
