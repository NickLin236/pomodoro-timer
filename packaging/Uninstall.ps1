#requires -Version 5.1
[CmdletBinding()]
param([switch]$RemoveData)

$ErrorActionPreference = 'Stop'
if (-not $env:LOCALAPPDATA) { throw 'This uninstaller requires a Windows user profile.' }
$installDirectory = Join-Path $env:LOCALAPPDATA 'Programs\PomodoroNative'
$installedExecutable = Join-Path $installDirectory 'Pomodoro.exe'
$running = @(Get-Process -Name 'Pomodoro' -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $installedExecutable })
if ($running.Count -ne 0) { throw 'Exit Pomodoro from its tray menu before uninstalling.' }

# Never remove another copy's sign-in entry, nor any other Run values.
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$expectedCommand = '"' + $installedExecutable + '" --background'
if (Test-Path -LiteralPath $runKey) {
    $runProperties = Get-ItemProperty -LiteralPath $runKey
    if ($runProperties.PSObject.Properties.Name -contains 'PomodoroNative') {
        if ($runProperties.PomodoroNative -eq $expectedCommand) {
            Remove-ItemProperty -LiteralPath $runKey -Name 'PomodoroNative'
        }
    }
}

$shortcutPath = Join-Path ([Environment]::GetFolderPath('Programs')) 'Pomodoro.lnk'
if (Test-Path -LiteralPath $shortcutPath) {
    $shell = New-Object -ComObject WScript.Shell
    try {
        $shortcut = $shell.CreateShortcut($shortcutPath)
        if ($shortcut.TargetPath -eq $installedExecutable) {
            Remove-Item -LiteralPath $shortcutPath
        }
    } finally {
        if ($shortcut) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) }
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
    }
}

# Remove only files installed by Install.ps1; retain unrelated files in the folder.
foreach ($file in @('Pomodoro.exe', 'Pomodoro.installing.exe')) {
    $path = Join-Path $installDirectory $file
    if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
}
if ((Test-Path -LiteralPath $installDirectory -PathType Container) -and
    @(Get-ChildItem -LiteralPath $installDirectory -Force).Count -eq 0) {
    Remove-Item -LiteralPath $installDirectory
}

if ($RemoveData) {
    $dataDirectory = Join-Path $env:LOCALAPPDATA 'PomodoroNative'
    # Deliberately avoid recursive deletion. Unknown files remain untouched.
    foreach ($file in @('state.json', 'state.json.bak',
                         'settings.json', 'settings.json.bak')) {
        $path = Join-Path $dataDirectory $file
        if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
    }
    if ((Test-Path -LiteralPath $dataDirectory -PathType Container) -and
        @(Get-ChildItem -LiteralPath $dataDirectory -Force).Count -eq 0) {
        Remove-Item -LiteralPath $dataDirectory
    }
    Write-Output 'Pomodoro was uninstalled and its known data files were removed.'
} else {
    Write-Output 'Pomodoro was uninstalled. Timer state and preferences were preserved.'
}
