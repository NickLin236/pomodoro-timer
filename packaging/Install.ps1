#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot 'Pomodoro.exe'),
    [switch]$NoShortcut
)

$ErrorActionPreference = 'Stop'
if (-not $env:LOCALAPPDATA) { throw 'This installer requires a Windows user profile.' }
$source = (Resolve-Path -LiteralPath $Executable).ProviderPath
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw 'Pomodoro.exe was not found.' }
$installDirectory = Join-Path $env:LOCALAPPDATA 'Programs\PomodoroNative'
$installedExecutable = Join-Path $installDirectory 'Pomodoro.exe'
$running = @(Get-Process -Name 'Pomodoro' -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $installedExecutable })
if ($running.Count -ne 0) { throw 'Exit Pomodoro from its tray menu before installing or updating.' }

New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
if ($source -ne $installedExecutable) {
    $temporaryExecutable = Join-Path $installDirectory 'Pomodoro.installing.exe'
    try {
        Copy-Item -LiteralPath $source -Destination $temporaryExecutable -Force
        Move-Item -LiteralPath $temporaryExecutable -Destination $installedExecutable -Force
    } finally {
        if (Test-Path -LiteralPath $temporaryExecutable) {
            Remove-Item -LiteralPath $temporaryExecutable -Force
        }
    }
}

if (-not $NoShortcut) {
    $programs = [Environment]::GetFolderPath('Programs')
    $shell = New-Object -ComObject WScript.Shell
    try {
        $shortcut = $shell.CreateShortcut((Join-Path $programs 'Pomodoro.lnk'))
        $shortcut.TargetPath = $installedExecutable
        $shortcut.WorkingDirectory = $installDirectory
        $shortcut.Description = 'Offline Pomodoro and countdown timer'
        $shortcut.Save()
    } finally {
        if ($shortcut) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) }
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
    }
}

Write-Output "Installed: $installedExecutable"
Write-Output 'Start when I sign in remains opt-in in the application settings.'
Write-Output 'Keep this release package to run Uninstall.ps1 later. Existing timer data is preserved.'
