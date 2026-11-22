[CmdletBinding()]
param(
    [ValidateSet("pass", "fail", "interrupt")][string]$Scenario,
    [string]$InstallDir,
    [string]$OutDir
)

$ErrorActionPreference = "Stop"
$acceptance = Join-Path $PSScriptRoot "../packaging/windows/native-windows-acceptance.ps1"

if ($Scenario) {
    function Join-Path($Path, $ChildPath) {
        if ($Path -eq $InstallDir -and $ChildPath -eq "Lightning.exe") { $ChildPath = "fixture.ps1" }
        Microsoft.PowerShell.Management\Join-Path -Path $Path -ChildPath $ChildPath
    }
    function Start-Process($FilePath, [switch]$PassThru, $RedirectStandardError, $RedirectStandardOutput) {
        "fixture log" | Set-Content -LiteralPath $RedirectStandardError
        "fixture stdout" | Set-Content -LiteralPath $RedirectStandardOutput
        $unlisted = Join-Path ([IO.Path]::GetDirectoryName($RedirectStandardError)) "unlisted.txt"
        "unlisted" | Set-Content -LiteralPath $unlisted
        $proc = [pscustomobject]@{ HasExited = ($Scenario -eq "fail") }
        $proc | Add-Member ScriptMethod CloseMainWindow { $true }
        $proc | Add-Member ScriptMethod WaitForExit { $true }
        return $proc
    }
    function Start-Sleep($Seconds, $Milliseconds) {
        if ($Scenario -eq "interrupt") { throw "fixture interrupted collection" }
    }
    function Get-CimInstance($ClassName) {
        [pscustomobject]@{ Name = "fixture"; CurrentHorizontalResolution = 800; CurrentVerticalResolution = 600 }
    }
    & $acceptance -InstallDir $InstallDir -OutDir $OutDir -AliveSeconds 1
    exit $LASTEXITCODE
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$work = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString("N"))
$install = Join-Path $work "fixture"
$out = Join-Path $work "reused [01]"
try {
    [IO.Directory]::CreateDirectory((Join-Path $install "plugins/platforms")) | Out-Null
    [IO.File]::WriteAllText((Join-Path $install "fixture.ps1"), @'
if ($args[0] -eq "--version") { "Lightning 0.11.0" }
else { "default_backend: rust"; "backends: rust"; "secret_store: windows-credential-manager" }
'@)
    [IO.File]::WriteAllText((Join-Path $install "plugins/platforms/qwindows.dll"), "")
    [IO.File]::WriteAllText((Join-Path $install "Qt6Core.dll"), "")
    [IO.Directory]::CreateDirectory((Join-Path $out "run-interrupted/nested")) | Out-Null
    $canaries = @("$out.zip", (Join-Path $out "private-canary.txt"),
                  (Join-Path $out "build-info.txt"), (Join-Path $out "run-interrupted/nested/private.txt"))
    foreach ($file in $canaries) { [IO.File]::WriteAllText($file, "original") }

    $shell = (Get-Process -Id $PID).Path
    $previous = @{}
    foreach ($case in "pass", "pass", "fail", "interrupt") {
        & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath -Scenario $case -InstallDir $install -OutDir $out
        if (($LASTEXITCODE -eq 0) -ne ($case -eq "pass")) { throw "Unexpected exit for $case" }
        $archives = @(Get-ChildItem -LiteralPath $out -Recurse -Filter diagnostics.zip)
        $new = @($archives | Where-Object { -not $previous.ContainsKey($_.FullName) })
        if ($new.Count -ne 1) { throw "Expected one new bundle for $case" }
        foreach ($path in $previous.Keys) {
            if ((Get-FileHash -LiteralPath $path).Hash -ne $previous[$path]) { throw "Previous bundle changed" }
        }
        $path = $new[0].FullName
        $previous[$path] = (Get-FileHash -LiteralPath $path).Hash
        $expected = @("build-info.txt", "lightning-app.log", "stdout.log", "acceptance-results.txt", "bundle-files.txt")
        if ($case -ne "interrupt") { $expected += "os-version.txt", "display.txt", "installed-files.csv" }
        $zip = [IO.Compression.ZipFile]::OpenRead($path)
        try {
            if (Compare-Object -CaseSensitive $expected @($zip.Entries.FullName)) { throw "Unexpected ZIP members for $case" }
            $reader = [IO.StreamReader]::new($zip.GetEntry("bundle-files.txt").Open())
            try { $inventory = $reader.ReadToEnd().Trim() -split '\r?\n' } finally { $reader.Dispose() }
            if (Compare-Object -CaseSensitive $expected $inventory) { throw "Incorrect inventory for $case" }
        } finally { $zip.Dispose() }
        foreach ($file in $canaries) {
            if ([IO.File]::ReadAllText($file) -ne "original") { throw "Pre-existing file changed" }
        }
    }
    Write-Host "Passed all four bundle collection scenarios."
} finally {
    if ([IO.Directory]::Exists($work)) { [IO.Directory]::Delete($work, $true) }
}
