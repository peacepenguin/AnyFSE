param(
    [ValidateSet('Install', 'Probe', 'Report', 'Remove')]
    [string]$Mode = 'Report',
    [ValidateSet('Highest', 'Limited')]
    [string]$Variant = 'Highest'
)

$ErrorActionPreference = 'Stop'
$taskPrefix = 'AnyFSE Logon Probe '
$outputDirectory = Join-Path $PSScriptRoot '..\artifacts\logon-probe'
$outputDirectory = [IO.Path]::GetFullPath($outputDirectory)

if ($Mode -eq 'Install') {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    foreach ($level in 'Highest', 'Limited') {
        $name = $taskPrefix + $level
        if (Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) {
            throw "Task already exists: $name. Use -Mode Remove before reinstalling."
        }
    }
    foreach ($level in 'Highest', 'Limited') {
        $action = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
            -Argument "-NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Mode Probe -Variant $level" `
            -WorkingDirectory $PSScriptRoot
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $identity.User.Value
        $trigger.Delay = 'PT5S'
        $principal = New-ScheduledTaskPrincipal -UserId $identity.User.Value -LogonType Interactive -RunLevel $level
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
            -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero)
        Register-ScheduledTask -TaskName ($taskPrefix + $level) -Action $action -Trigger $trigger `
            -Principal $principal -Settings $settings -Description 'Temporary Xbox-mode logon timing probe; writes diagnostic samples only.' | Out-Null
    }
    Write-Output "Installed Highest and Limited logon probes. Output: $outputDirectory"
    exit
}

if ($Mode -eq 'Remove') {
    foreach ($level in 'Highest', 'Limited') {
        $task = Get-ScheduledTask -TaskName ($taskPrefix + $level) -ErrorAction SilentlyContinue
        if ($task) {
            $task | Stop-ScheduledTask
            $task | Unregister-ScheduledTask -Confirm:$false
        }
    }
    Write-Output 'Removed probe tasks; captured logs are preserved.'
    exit
}

if ($Mode -eq 'Report') {
    foreach ($level in 'Highest', 'Limited') {
        $task = Get-ScheduledTask -TaskName ($taskPrefix + $level) -ErrorAction SilentlyContinue
        if ($task) {
            $info = $task | Get-ScheduledTaskInfo
            [pscustomobject]@{ Task = $task.TaskName; State = $task.State; LastRun = $info.LastRunTime; Result = $info.LastTaskResult }
        }
    }
    Get-ChildItem $outputDirectory -Filter '*.jsonl' -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Output "`n$($_.FullName)"
        Get-Content $_.FullName | Select-Object -First 2
        Get-Content $_.FullName | Select-Object -Last 2
    }
    exit
}

New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$started = [DateTimeOffset]::Now
$session = (Get-Process -Id $PID).SessionId
$outputFile = Join-Path $outputDirectory ("{0}-{1}-{2}.jsonl" -f $started.ToString('yyyyMMdd-HHmmss'), $Variant, $PID)
function Write-Sample($sample) {
    $sample | ConvertTo-Json -Compress -Depth 4 | Add-Content -LiteralPath $outputFile -Encoding UTF8
}
# Write before compiling the API wrapper so even initialization failures leave evidence.
Write-Sample ([ordered]@{ Event = 'ProcessStarted'; Time = $started.ToString('o'); Variant = $Variant; PID = $PID; Session = $session })
try {
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class LogonProbeGaming {
    [DllImport("api-ms-win-gaming-experience-l1-1-0.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsGamingFullScreenExperienceActive();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern System.IntPtr FindWindow(string name, string title);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(System.IntPtr window, out uint pid);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern System.IntPtr SendMessageTimeout(System.IntPtr window, uint message, System.IntPtr w, System.IntPtr l,
        uint flags, uint timeout, out System.IntPtr result);
    [DllImport("user32.dll")]
    public static extern System.IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll")]
    public static extern bool CloseDesktop(System.IntPtr desktop);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool GetUserObjectInformation(System.IntPtr handle, int index, System.Text.StringBuilder value, uint size, out uint needed);
    public static string InputDesktop() {
        var desktop = OpenInputDesktop(0, false, 1);
        if (desktop == System.IntPtr.Zero) return null;
        try {
            var name = new System.Text.StringBuilder(256);
            uint needed;
            return GetUserObjectInformation(desktop, 2, name, 512, out needed) ? name.ToString() : null;
        } finally { CloseDesktop(desktop); }
    }
}
'@
    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
    for ($i = 0; $i -lt 120; $i++) {
        $fse = $null
        $apiError = $null
        try { $fse = [LogonProbeGaming]::IsGamingFullScreenExperienceActive() }
        catch { $apiError = $_.Exception.Message }
        $listenerWindow = [LogonProbeGaming]::FindWindow('HIDListener', $null)
        [uint32]$listenerPid = 0
        [void][LogonProbeGaming]::GetWindowThreadProcessId($listenerWindow, [ref]$listenerPid)
        $reply = [IntPtr]::Zero
        $responsive = $false
        if ($listenerWindow -ne [IntPtr]::Zero) {
            $responsive = [LogonProbeGaming]::SendMessageTimeout($listenerWindow, 0, [IntPtr]::Zero, [IntPtr]::Zero, 2, 500, [ref]$reply) -ne [IntPtr]::Zero
        }
        $explorer = @(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -EQ $session | ForEach-Object {
            [ordered]@{ PID = $_.Id; Started = $_.StartTime.ToUniversalTime().ToString('o') }
        })
        Write-Sample ([ordered]@{
            Event = 'Sample'; Time = [DateTimeOffset]::Now.ToString('o'); BootUtc = $boot
            Variant = $Variant; PID = $PID; Session = $session; FSE = $fse; ApiError = $apiError; Explorer = $explorer
            InputDesktop = [LogonProbeGaming]::InputDesktop()
            Listener = [ordered]@{ Window = $listenerWindow.ToInt64(); PID = $listenerPid; Responsive = $responsive }
        })
        Start-Sleep -Seconds 5
    }
    Write-Sample ([ordered]@{ Event = 'Completed'; Time = [DateTimeOffset]::Now.ToString('o') })
}
catch {
    Write-Sample ([ordered]@{ Event = 'Error'; Time = [DateTimeOffset]::Now.ToString('o'); Error = $_.Exception.Message })
    exit 1
}
