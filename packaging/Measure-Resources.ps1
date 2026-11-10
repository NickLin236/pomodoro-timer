#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $env:LOCALAPPDATA 'Programs\PomodoroNative\Pomodoro.exe'),
    [int]$ProcessId = 0,
    [ValidateRange(1, 86400)][int]$DurationSeconds = 300,
    [ValidateRange(1, 3600)][int]$IntervalSeconds = 1,
    [string]$CsvPath = (Join-Path (Get-Location) 'Pomodoro-resources.csv'),
    [switch]$Launch
)

$ErrorActionPreference = 'Stop'
if ($Launch -and $ProcessId -ne 0) { throw 'Choose either -Launch or -ProcessId.' }
if ($ProcessId -ne 0) {
    $measuredProcess = Get-Process -Id $ProcessId
} else {
    $executablePath = (Resolve-Path -LiteralPath $Executable).ProviderPath
    $candidates = @(Get-Process -Name 'Pomodoro' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $executablePath })
    if ($candidates.Count -gt 1) { throw 'Multiple matching processes; specify -ProcessId.' }
    if ($candidates.Count -eq 1) {
        $measuredProcess = $candidates[0]
    } elseif ($Launch) {
        $measuredProcess = Start-Process -FilePath $executablePath -PassThru
        Start-Sleep -Milliseconds 500
    } else {
        throw 'No matching process. Start a Release build, or pass -Launch.'
    }
}
if ($measuredProcess.HasExited) { throw 'The selected process exited; choose the running instance.' }

if (-not ('PomodoroMetrics.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace PomodoroMetrics {
    public static class Native {
        [DllImport("user32.dll", SetLastError = true)]
        public static extern uint GetGuiResources(IntPtr process, uint flag);
    }
}
'@
}

$destination = [IO.Path]::GetFullPath($CsvPath)
if (Test-Path -LiteralPath $destination) { throw "Output already exists: $destination" }
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$previousElapsed = 0.0
$measuredProcess.Refresh()
$previousCpu = $measuredProcess.TotalProcessorTime.TotalSeconds
$firstSample = $true
$processorCount = [Environment]::ProcessorCount

Write-Output "Measuring process $($measuredProcess.Id) for $DurationSeconds seconds."
Write-Output 'Use a Release build. Minimize/restore and exercise timers during measurement as needed.'
while ($true) {
    $measuredProcess.Refresh()
    if ($measuredProcess.HasExited) { Write-Warning 'The measured process exited.'; break }
    $elapsed = $stopwatch.Elapsed.TotalSeconds
    $cpu = $measuredProcess.TotalProcessorTime.TotalSeconds
    $interval = $elapsed - $previousElapsed
    $cpuPercent = if ($firstSample -or $interval -le 0) { 0.0 } else {
        100.0 * ($cpu - $previousCpu) / $interval / $processorCount
    }
    $row = [pscustomobject][ordered]@{
        timestampUtc = [DateTime]::UtcNow.ToString('o')
        elapsedSeconds = [Math]::Round($elapsed, 3)
        processId = $measuredProcess.Id
        privateMiB = [Math]::Round($measuredProcess.PrivateMemorySize64 / 1MB, 3)
        workingSetMiB = [Math]::Round($measuredProcess.WorkingSet64 / 1MB, 3)
        cpuPercentMachine = [Math]::Round($cpuPercent, 4)
        threads = $measuredProcess.Threads.Count
        handles = $measuredProcess.HandleCount
        gdiHandles = [PomodoroMetrics.Native]::GetGuiResources($measuredProcess.Handle, 0)
        userHandles = [PomodoroMetrics.Native]::GetGuiResources($measuredProcess.Handle, 1)
    }
    if ($firstSample) {
        $row | Export-Csv -LiteralPath $destination -NoTypeInformation -Encoding UTF8
    } else {
        $row | Export-Csv -LiteralPath $destination -NoTypeInformation -Encoding UTF8 -Append
    }
    $firstSample = $false
    $previousElapsed = $elapsed
    $previousCpu = $cpu
    if ($elapsed -ge $DurationSeconds) { break }
    $sleepMs = [Math]::Min($IntervalSeconds * 1000,
        [Math]::Max(1, ($DurationSeconds - $elapsed) * 1000))
    Start-Sleep -Milliseconds ([int]$sleepMs)
}
$stopwatch.Stop()
Write-Output "Measurements saved to $destination. The application remains running."
