[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$SkipBuild,
    [string]$GameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\Enshrouded',
    [int]$WaitSeconds = 20
)
$ErrorActionPreference = 'Stop'
$projectDirectory = Split-Path $PSScriptRoot -Parent
$gameDirectoryPath = (Resolve-Path -LiteralPath $GameDirectory).Path
if (-not (Test-Path -LiteralPath (Join-Path $gameDirectoryPath 'enshrouded.exe'))) { throw 'This is not an Enshrouded directory.' }
if (-not $SkipBuild) {
    & (Join-Path $projectDirectory 'build-hot.bat')
    if ($LASTEXITCODE -ne 0) { throw 'Hot build failed. Nothing installed.' }
}
$sourceDirectory = Join-Path $projectDirectory 'bin-hot'
$loaderDestination = Join-Path $gameDirectoryPath 'wsock32.dll'
$runtimeDestination = Join-Path $gameDirectoryPath 'Find.runtime.dll'
$statusPath = Join-Path $gameDirectoryPath 'Find.hotload.status'
$running = @(Get-Process enshrouded -ErrorAction SilentlyContinue).Count -gt 0
if ($Install) {
    if ($running) { throw 'Close Enshrouded once before installing the permanent loader.' }
    $backupPath = Join-Path $gameDirectoryPath ('wsock32.before-hotloader.' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.dll')
    if (Test-Path -LiteralPath $loaderDestination) { Copy-Item -LiteralPath $loaderDestination -Destination $backupPath }
    Copy-Item -LiteralPath (Join-Path $sourceDirectory 'Find.runtime.dll') -Destination $runtimeDestination -Force
    Copy-Item -LiteralPath (Join-Path $sourceDirectory 'wsock32.dll') -Destination $loaderDestination -Force
    Write-Output 'Installed. Start Enshrouded normally, then enter a world.'
    return
}
if (-not $running) { throw 'Use -Install for the first installation while the game is closed.' }
$installedHash = (Get-FileHash -LiteralPath $loaderDestination).Hash
$builtHash = (Get-FileHash -LiteralPath (Join-Path $sourceDirectory 'wsock32.dll')).Hash
if ($installedHash -ne $builtHash) { throw 'The permanent loader differs from this build. Restart and run -Install once; no runtime was replaced.' }
$previousStatus = if (Test-Path -LiteralPath $statusPath) { Get-Content -LiteralPath $statusPath -Raw } else { '' }
$stagedRuntime = Join-Path $gameDirectoryPath ('Find.runtime.stage.' + [Guid]::NewGuid().ToString('N') + '.dll')
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'Find.runtime.dll') -Destination $stagedRuntime
# A single rename publishes the complete candidate; loaded shadow copies never lock this file.
Move-Item -LiteralPath $stagedRuntime -Destination $runtimeDestination -Force
[IO.File]::WriteAllText((Join-Path $gameDirectoryPath 'Find.reload'), 'reload')
$deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
while ([DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 250
    $currentStatus = try { Get-Content -LiteralPath $statusPath -Raw -ErrorAction Stop } catch { '' }
    if ($currentStatus -and $currentStatus -ne $previousStatus -and ($currentStatus.StartsWith('active generation ') -or $currentStatus.StartsWith('error:') -or $currentStatus.StartsWith('busy:'))) {
        Write-Output $currentStatus.Trim()
        if (-not $currentStatus.StartsWith('active generation ')) { throw 'Candidate was not activated. The previous runtime remains active.' }
        return
    }
}
Write-Output 'Reload requested but not yet confirmed. Close the Find dialog and remain in a world; read Find.hotload.status before requesting again.'
