[CmdletBinding()]
param(
    [ValidateSet("pass", "fail", "interrupt", "nolog", "forcekill", "pe-truncated")][string]$Scenario,
    [string]$InstallDir,
    [string]$OutDir
)

$ErrorActionPreference = "Stop"
$acceptance = Join-Path $PSScriptRoot "../packaging/windows/native-windows-acceptance.ps1"

if ($Scenario) {
    # The fixture "exe" is a PowerShell script whose first bytes are a PE header.
    function MZ { }
    function Join-Path($Path, $ChildPath) {
        if ($Path -eq $InstallDir -and $ChildPath -eq "Lightning.exe") {
            $ChildPath = if ($Scenario -eq "pe-truncated") { "fixture-truncated.ps1" } else { "fixture.ps1" }
        }
        Microsoft.PowerShell.Management\Join-Path -Path $Path -ChildPath $ChildPath
    }
    function Start-Process($FilePath, [switch]$PassThru, $RedirectStandardError, $RedirectStandardOutput) {
        $appLog = if ($Scenario -eq "nolog") { "fixture log" } else { @(
            "lightning: scene graph backend=d3d11 software=0 platform=windows refreshHz=60.0 dpr=1.00",
            'matrix.rust: Rust SDK backend loaded: "matrix-rust-sdk" version "0.18.0" supports_e2ee= true',
            "matrix.secret.wincred: Windows secure credential store active") }
        $appLog | Set-Content -LiteralPath $RedirectStandardError
        "fixture stdout" | Set-Content -LiteralPath $RedirectStandardOutput
        $unlisted = Join-Path ([IO.Path]::GetDirectoryName($RedirectStandardError)) "unlisted.txt"
        "unlisted" | Set-Content -LiteralPath $unlisted
        $proc = [pscustomobject]@{ HasExited = ($Scenario -eq "fail"); ExitCode = 0; Stuck = ($Scenario -eq "forcekill") }
        $proc | Add-Member ScriptMethod CloseMainWindow { if (-not $this.Stuck) { $this.HasExited = $true }; $true }
        $proc | Add-Member ScriptMethod WaitForExit { -not $this.Stuck }
        $proc | Add-Member ScriptMethod Kill { $this.HasExited = $true; $this.ExitCode = -1 }
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
    $code = [Text.Encoding]::ASCII.GetBytes(@'
#>
if ($args[0] -eq "--version") { "Lightning 0.11.0" }
else { "source: 0123456789abcdef"; "default_backend: rust"; "backends: rust"; "secret_store: windows-credential-manager" }
'@)
    # MZ, e_lfanew at 0x3C, "PE\0\0", Subsystem at e_lfanew+92; the rest sits in a block comment.
    foreach ($fixture in @(@{ Name = "fixture.ps1"; PeOffset = 64 }, @{ Name = "fixture-truncated.ps1"; PeOffset = 4000 })) {
        $head = [byte[]]::new(160)
        for ($i = 0; $i -lt $head.Length; $i++) { $head[$i] = 0x78 }
        [Text.Encoding]::ASCII.GetBytes("MZ;<#").CopyTo($head, 0)
        [BitConverter]::GetBytes([int]$fixture.PeOffset).CopyTo($head, 0x3C)
        ([byte[]](0x50, 0x45, 0, 0)).CopyTo($head, 64)
        ([byte[]](2, 0)).CopyTo($head, 64 + 92)
        [IO.File]::WriteAllBytes((Join-Path $install $fixture.Name), $head + $code)
    }
    [IO.File]::WriteAllText((Join-Path $install "plugins/platforms/qwindows.dll"), "")
    [IO.File]::WriteAllText((Join-Path $install "Qt6Core.dll"), "")
    [IO.Directory]::CreateDirectory((Join-Path $out "run-interrupted/nested")) | Out-Null
    $canaries = @("$out.zip", (Join-Path $out "private-canary.txt"),
                  (Join-Path $out "build-info.txt"), (Join-Path $out "run-interrupted/nested/private.txt"))
    foreach ($file in $canaries) { [IO.File]::WriteAllText($file, "original") }

    $shell = (Get-Process -Id $PID).Path
    $previous = @{}
    $cases = [ordered]@{}
    foreach ($case in "pass", "pass", "fail", "interrupt", "nolog", "forcekill", "pe-truncated") {
        $want = @{ "pass" = @(0, "PASS"); "pe-truncated" = @(2, "INCONCLUSIVE") }[$case]
        if (-not $want) { $want = @(1, "FAIL") }
        & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath -Scenario $case -InstallDir $install -OutDir $out
        if ($LASTEXITCODE -ne $want[0]) { throw "Exit $LASTEXITCODE for $case, expected $($want[0])" }
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
            $reader = [IO.StreamReader]::new($zip.GetEntry("acceptance-results.txt").Open())
            try { $report = $reader.ReadToEnd() } finally { $reader.Dispose() }
            if ($report -notmatch "^ACCEPTANCE: $($want[1])\r?\n") { throw "Verdict for $case is not $($want[1])" }
            if ($report -notmatch "(?m)^checks: \d+ passed, \d+ failed, \d+ not tested, \d+ total") { throw "No counts for $case" }
            if ($report -notmatch "(?m)^artifact: Lightning.exe sha256 [0-9A-F]{64}, source ") { throw "No artifact identity for $case" }
            $cases[$case] = $report
        } finally { $zip.Dispose() }
        foreach ($file in $canaries) {
            if ([IO.File]::ReadAllText($file) -ne "original") { throw "Pre-existing file changed" }
        }
    }
    $checks = @(
        @("pass", "(?m)^checks: 22 passed, 0 failed, 1 not tested, 23 total \(22 of 22 required recorded\)"),
        @("pass", "(?m)^artifact: .*, source 0123456789abcdef$"),
        @("pass", "(?m)^NOT TESTED log: account cache opened .*\(optional\)$"),
        @("nolog", "(?m)^FAIL +log: Rust SDK backend loaded$"),
        @("nolog", "(?m)^FAIL +log: windows platform plugin rendering$"),
        @("nolog", "(?m)^FAIL +log: Windows secure store active$"),
        @("forcekill", "(?m)^FAIL +app exits on a close request within 10 s \(not killed\), exit code -1$"),
        @("fail", "(?m)^NOT TESTED app exits on a close request"),
        @("pe-truncated", "(?m)^NOT TESTED production exe is GUI subsystem \(2\) \[PE read failed"),
        @("pe-truncated", "(?m)^checks: 21 passed, 0 failed, 2 not tested"))
    foreach ($c in $checks) { if ($cases[$c[0]] -notmatch $c[1]) { throw "$($c[0]) report lacks $($c[1]):`n$($cases[$c[0]])" } }
    Write-Host "Passed all seven acceptance scenarios and $($checks.Count) report checks."
} finally {
    if ([IO.Directory]::Exists($work)) { [IO.Directory]::Delete($work, $true) }
}
