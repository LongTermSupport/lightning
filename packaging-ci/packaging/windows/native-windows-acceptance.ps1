<#
.SYNOPSIS
    Native Windows acceptance test for a Lightning test build.

.DESCRIPTION
    Operator-run on a REAL Windows host (Wine cannot prove any of the native
    behaviour this checks). It verifies the corrected Windows build:

      * the production Lightning.exe is a GUI-subsystem PE (no console flash);
      * --version and --build-info work, and --build-info reports the Rust
        default backend and the Windows Credential Manager secret store;
      * the app STARTS normally with no DISPLAY / WAYLAND_DISPLAY /
        QT_QPA_PLATFORM set, stays alive for a bounded window, and closes;
      * the captured diagnostic log positively shows the windows platform
        plugin, the Rust backend and the Windows secure store, and shows NONE
        of: the insecure QSettings fallback, an invalid cache path, a cache
        open failure, an "Invalid window handle" shutdown warning, a QML
        binding loop, a pagination storm, or a startup failure;
      * the app exits on a normal close request, without being killed;
      * a per-run diagnostic ZIP lists its entries in bundle-files.txt.
        Review the raw app log, stdout and installed paths before sharing.

    Every check ends PASS, FAIL or NOT TESTED. The verdict is FAIL (exit 1)
    on any failure, INCONCLUSIVE (exit 2) when a required check could not be
    run, and PASS (exit 0) only when every required check ran and passed.
    The account cache opens only once an account is signed in, so its check
    is optional: run in a disposable Windows user account, sign in a test
    account and re-run to get it.

.PARAMETER InstallDir
    Directory containing Lightning.exe (e.g. the portable folder or
    %LOCALAPPDATA%\Programs\Lightning). Defaults to the script's folder.

.PARAMETER OutDir
    Parent of the new run directory and its diagnostic bundle. Defaults to a temp folder.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File native-windows-acceptance.ps1 `
        -InstallDir "$env:LOCALAPPDATA\Programs\Lightning"
#>
[CmdletBinding()]
param(
    [string]$InstallDir = $PSScriptRoot,
    [string]$OutDir = (Join-Path $env:TEMP ("lightning-acceptance-" + (Get-Date -Format "yyyyMMdd-HHmmss"))),
    [int]$AliveSeconds = 8
)

$ErrorActionPreference = "Stop"
$exe = Join-Path $InstallDir "Lightning.exe"
$results = [System.Collections.Generic.List[object]]::new()
# Bump when a required check is added: a run that records fewer is INCONCLUSIVE.
$requiredChecks = 22

function Record([string]$name, [string]$status, [switch]$Optional) {
    $script:results.Add([pscustomobject]@{ Status = $status; Name = $name; Required = -not $Optional })
}
function Check([string]$name, [bool]$ok, [switch]$Optional) {
    Record $name $(if ($ok) { "PASS" } else { "FAIL" }) -Optional:$Optional
}

$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
[System.IO.Directory]::CreateDirectory($OutDir) | Out-Null
$runDir = (New-Item -ItemType Directory -ErrorAction Stop -Path (
    Join-Path $OutDir ("run-" + [Guid]::NewGuid().ToString("N")))).FullName
$log = Join-Path $runDir "lightning-app.log"
$buildInfoLog = Join-Path $runDir "build-info.txt"
$zip = Join-Path $runDir "diagnostics.zip"

function Invoke-AcceptanceChecks {
    # --- 1. Installed files -----------------------------------------------------
    Check "Lightning.exe present" (Test-Path -LiteralPath $exe)
    Check "qwindows platform plugin present" (Test-Path -LiteralPath (Join-Path $InstallDir "plugins\platforms\qwindows.dll"))
    Check "Qt6Core.dll present" (Test-Path -LiteralPath (Join-Path $InstallDir "Qt6Core.dll"))

    # --- 2/3. --version and --build-info ----------------------------------------
    $versionOut = & $exe --version 2>&1 | Out-String
    Check "--version prints Lightning" ($versionOut -match "Lightning\s+\d+\.\d+\.\d+")

    $buildInfoOut = & $exe --build-info 2>&1 | Out-String
    $buildInfoOut | Set-Content -LiteralPath $buildInfoLog -Encoding UTF8
    Check "--build-info default_backend: rust" ($buildInfoOut -match "(?m)^default_backend:\s*rust\s*$")
    Check "--build-info secret_store: windows-credential-manager" ($buildInfoOut -match "(?m)^secret_store:\s*windows-credential-manager\s*$")
    Check "--build-info lists the rust backend" ($buildInfoOut -match "(?m)^backends:\s*.*rust")

    # --- 4. Production PE subsystem = GUI ---------------------------------------
    # Read the PE header directly: DOS e_lfanew -> COFF -> Optional header Subsystem
    # (offset 68). 2 = Windows GUI, 3 = Windows console (CUI).
    try {
        $bytes = [System.IO.File]::ReadAllBytes($exe)
        $peOff = [BitConverter]::ToInt32($bytes, 0x3C)
        $isPe = $bytes[0] -eq 0x4D -and $bytes[1] -eq 0x5A -and
            [BitConverter]::ToUInt32($bytes, $peOff) -eq 0x00004550
        $subsystem = [BitConverter]::ToUInt16($bytes, $peOff + 4 + 20 + 68)
        Check "production exe is GUI subsystem (2)" ($isPe -and $subsystem -eq 2)
    } catch {
        Record "production exe is GUI subsystem (2) [PE read failed: $($_.Exception.Message)]" "NOT TESTED"
    }

    # --- 5/6. Start with NO display env; stay alive; then close -----------------
    $env:DISPLAY = $null; Remove-Item Env:\DISPLAY -ErrorAction SilentlyContinue
    Remove-Item Env:\WAYLAND_DISPLAY -ErrorAction SilentlyContinue
    Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue
    # Route Qt category logs to the redirected stderr handle so we can inspect them.
    $env:QT_FORCE_STDERR_LOGGING = "1"
    $env:QT_LOGGING_RULES = "matrix.*=true"

    $proc = Start-Process -FilePath $exe -PassThru `
        -RedirectStandardError $log -RedirectStandardOutput (Join-Path $runDir "stdout.log")
    $closeName = "app exits on a close request within 10 s (not killed)"
    try {
        Start-Sleep -Seconds $AliveSeconds
        $alive = -not $proc.HasExited
        Check "app stays alive $AliveSeconds s with no DISPLAY set" $alive
    } finally {
        if (-not $proc.HasExited) {
            $proc.CloseMainWindow() | Out-Null
            $closed = $proc.WaitForExit(10000)
            if (-not $closed) { $proc.Kill(); $proc.WaitForExit() }
            Check "$closeName, exit code $($proc.ExitCode)" ($closed -and $proc.ExitCode -eq 0)
        }
    }
    if (-not $alive) { Record $closeName "NOT TESTED" }

    Start-Sleep -Milliseconds 500
    $logText = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { "" }

    # --- 7/8. Positive log markers ---------------------------------------------
    Check "no 'no graphical display available' rejection" (-not ($logText -match "no graphical display available"))
    Check "log: windows platform plugin rendering" ($logText -match "scene graph backend=\S+ software=\d platform=windows\b")
    Check "log: Rust SDK backend loaded" ($logText -match "Rust SDK backend loaded")
    Check "no matrix.http session" (-not ($logText -match "matrix\.http:\s*session restored"))
    Check "no insecure QSettings fallback warning" (-not ($logText -match "InsecureFallbackSecretStore active"))
    Check "log: Windows secure store active" ($logText -match "Windows secure credential store active")
    Check "no invalid account cache path" (-not ($logText -match "refusing invalid account cache path"))
    Check "no cache open failure" (-not ($logText -match "matrix\.cache.*open failed"))
    if ($logText -match "matrix\.cache.*cache open") { Record "log: account cache opened" "PASS" -Optional }
    else { Record "log: account cache opened (needs a signed-in account)" "NOT TESTED" -Optional }

    # --- 10. Negative log markers (regressions) --------------------------------
    Check "no 'Invalid window handle' shutdown race" (-not ($logText -match "Invalid window handle"))
    Check "no QML binding loop" (-not ($logText -match "Binding loop detected"))
    Check "no pagination storm" (($logText | Select-String "pagination requested" -AllMatches).Matches.Count -lt 40)
    Check "no libsecret plaintext fallback line" (-not ($logText -match "using insecure QSettings fallback"))

    "$([System.Environment]::OSVersion.VersionString)" | Set-Content -LiteralPath (Join-Path $runDir "os-version.txt")
    try {
        Get-CimInstance Win32_VideoController |
            Select-Object Name, CurrentHorizontalResolution, CurrentVerticalResolution |
            Format-List | Out-File -LiteralPath (Join-Path $runDir "display.txt")
    } catch { }
    Get-ChildItem -Recurse -LiteralPath $InstallDir | Select-Object FullName, Length |
        Export-Csv -NoTypeInformation -LiteralPath (Join-Path $runDir "installed-files.csv")
}

$completed = $false
try {
    Invoke-AcceptanceChecks
    $completed = $true
} finally {
    if (-not $completed) { Check "diagnostic collection completed" $false }
    $required = @($results | Where-Object Required)
    $failed = @($results | Where-Object Status -eq "FAIL").Count
    $notTested = @($required | Where-Object Status -eq "NOT TESTED").Count
    $verdict = if ($failed -gt 0) { "FAIL" }
        elseif ($notTested -gt 0 -or $required.Count -lt $requiredChecks) { "INCONCLUSIVE" }
        else { "PASS" }
    $exeHash = try { (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash } catch { "unavailable" }
    $source = if ((Test-Path -LiteralPath $buildInfoLog) -and
                  ((Get-Content -LiteralPath $buildInfoLog -Raw) -match "(?m)^source:\s*(\S+)")) { $Matches[1] } else { "unknown" }
    $summary = @(
        "ACCEPTANCE: $verdict",
        "artifact: Lightning.exe sha256 $exeHash, source $source",
        ("checks: {0} passed, {1} failed, {2} not tested, {3} total ({4} of {5} required recorded)" -f
            @($results | Where-Object Status -eq "PASS").Count, $failed,
            @($results | Where-Object Status -eq "NOT TESTED").Count, $results.Count, $required.Count, $requiredChecks)
    )
    $lines = $summary + "" + ($results | ForEach-Object {
        "{0,-10} {1}{2}" -f $_.Status, $_.Name, $(if ($_.Required) { "" } else { " (optional)" }) })
    $lines | Set-Content -LiteralPath (Join-Path $runDir "acceptance-results.txt")
    $files = @("acceptance-results.txt", "build-info.txt", "lightning-app.log", "stdout.log",
               "os-version.txt", "display.txt", "installed-files.csv") | Where-Object {
        [System.IO.File]::Exists((Join-Path $runDir $_))
    }
    $files = @($files) + "bundle-files.txt"
    $files | Set-Content -LiteralPath (Join-Path $runDir "bundle-files.txt")

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::Open("$zip.partial", "Create")
    try {
        foreach ($name in $files) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, (Join-Path $runDir $name), $name) | Out-Null
        }
    } finally {
        $archive.Dispose()
    }
    [System.IO.File]::Move("$zip.partial", $zip)
    Write-Host "Diagnostic bundle: $zip"
}

Write-Host ""
$lines | ForEach-Object { Write-Host $_ }
switch ($verdict) {
    "PASS" { exit 0 }
    "FAIL" { exit 1 }
    default { exit 2 }
}
