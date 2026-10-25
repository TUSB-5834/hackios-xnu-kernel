Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\Start-iOS.ps1')
$script:Package = Get-Content -LiteralPath (Join-Path $script:Root 'package.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Assert-Payload
Assert-Apfs (Get-PackagePath $script:Package.system)
Assert-Apfs (Get-PackagePath $script:Package.boot)
Write-Host 'PASS: actual XNUP entries match actual exported kernel/Boot; APFS geometries match.'

$script:RealRoot = $script:Root
$script:TestCases = @()
$script:Calls = [Collections.Generic.List[string]]::new()
$script:FullChecks = 0
$script:Followed = $false
$script:State = @{}
function Test-Files([bool]$ForceHash, [bool]$ToolsOnly = $false) {
    if (-not $ToolsOnly) { $script:FullChecks++ }
}
function Assert-Payload {}
function Assert-Apfs([string]$Path) {}
function Follow-Logs { $script:Followed = $true }
function Invoke-Fastboot([string[]]$Arguments, [string]$Serial = '', [int]$TimeoutSeconds = 15) {
    Assert-Command $Arguments $Serial
    $command = $Arguments -join ' '
    $script:Calls.Add($command)
    if ($command -eq 'devices -l') {
        if ($script:State.noDevice) { return '' }
        return "$($script:State.device)`tfastboot usb:test`n"
    }
    if ($Arguments[0] -eq 'boot') { $script:State.device = 'xnuloader'; return 'OKAY' }
    if ($Arguments[0] -eq 'stage' -or $command -in @('oem battery-probe','oem ramdisk','oem ramdisk none')) { return 'OKAY' }
    if ($command -eq 'oem persist') {
        $script:State.runtime = if ($script:State.rejectKernel) { 'stopped' } else { 'running' }
        return 'OKAY'
    }
    if ($Arguments[0] -eq 'get_staged') {
        if ($script:State.exportFails) { throw 'Simulated disconnected USB' }
        [IO.File]::WriteAllText($Arguments[1], "EARLY KERNEL EXCEPTION: simulated evidence", $script:Utf8)
        return 'OKAY'
    }
    if ($Arguments[0] -ne 'getvar') { throw "Unexpected simulated command: $command" }
    $name = $Arguments[1]
    $values = @{'product'=$script:State.product; 'platform'='dada';
        'version-bootloader'=$script:State.loader; 'run-control'='persistent-follow-v1';
        'log-protocol'='DL1-base64-fastboot-INFO'; 'auto-return'='disabled';
        'battery-service'='dada-mynewt-bq27z561-read-v4'; 'runtime'=$script:State.runtime;
        'battery-status'=$script:State.batteryStatus; 'battery-soc'='62';
        'xnu-memory-limit-mib'='8192'; 'xnu-memory-mib'=$script:State.memory;
        'result'='early-exception'; 'return-reason'='early-exception'; 'kernel-heartbeat'='0'}
    if (-not $values.ContainsKey($name)) { throw "Unexpected simulated variable: $name" }
    return "${name}: $($values[$name])`nFinished. Total time: 0.001s`n"
}

function Assert-Test([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "TEST FAILED: $Message" }
}

foreach ($case in @('cold-start','already-running','no-device','wrong-product','wrong-loader','battery-failed','low-memory','verify-offline',
    'kernel-returned','failure-export-disconnected','logs-after-return')) {
    $script:Calls.Clear()
    $script:Followed = $false
    $script:FullChecks = 0
    $script:Mode = 'Start'
    $script:State = @{noDevice=$false; device='28c3d049'; product='dada';
        loader='dada-touch-spi-v16-mynewt-r16h'; runtime='stopped'; batteryStatus='0'; memory='8192'; rejectKernel=$false; exportFails=$false}
    switch ($case) {
        'already-running' { $script:State.device='xnuloader'; $script:State.runtime='running' }
        'no-device' { $script:State.noDevice=$true }
        'wrong-product' { $script:State.product='another-phone' }
        'wrong-loader' { $script:State.device='xnuloader'; $script:State.loader='another-loader' }
        'battery-failed' { $script:State.batteryStatus='7' }
        'low-memory' { $script:State.memory='4096' }
        'verify-offline' { $script:Mode='Verify' }
        'kernel-returned' { $script:State.rejectKernel=$true }
        'failure-export-disconnected' { $script:State.rejectKernel=$true; $script:State.exportFails=$true }
        'logs-after-return' { $script:State.device='xnuloader'; $script:Mode='Logs' }
    }
    $code = Start-Main
    $commands = @($script:Calls.ToArray())
    $stages = @($commands | Where-Object { $_.StartsWith('stage ') })
    $boots = @($commands | Where-Object { $_.StartsWith('boot ') })
    $started = $commands -contains 'oem persist'
    if ($case -eq 'cold-start') {
        Assert-Test ($code -eq 0 -and $boots.Count -eq 1 -and $stages.Count -eq 2 -and $started -and $script:Followed) $case
        Assert-Test ($stages[0] -eq ('stage ' + (Get-PackagePath $script:Package.system))) 'System must upload first'
        Assert-Test ($stages[1] -eq ('stage ' + (Get-PackagePath $script:Package.payload))) 'Kernel/Boot must upload second'
    } elseif ($case -in @('kernel-returned','failure-export-disconnected','logs-after-return')) {
        Assert-Test ($code -eq 1 -and -not $script:Followed) $case
        Assert-Test (@($commands | Where-Object { $_.StartsWith('get_staged ') }).Count -eq 1) 'Export final log exactly once'
        Assert-Test ($commands -contains 'getvar return-reason' -and $commands -notcontains 'oem log') 'Preserve returned kernel snapshot'
        if ($case -eq 'failure-export-disconnected') {
            Assert-Test (Test-Path -LiteralPath (Join-Path $script:Session 'log-export-error.txt')) 'Record export failure'
        } else {
            Assert-Test (Test-Path -LiteralPath (Join-Path $script:Session 'kernel-final.log')) 'Save final kernel evidence'
        }
        if ($case -eq 'logs-after-return') { Assert-Test (-not $started -and $stages.Count -eq 0) 'Logs must not start a kernel' }
    } elseif ($case -eq 'already-running') {
        Assert-Test ($code -eq 0 -and $stages.Count -eq 0 -and $boots.Count -eq 0 -and -not $started -and $script:Followed) $case
    } elseif ($case -eq 'verify-offline') {
        Assert-Test ($code -eq 0 -and $commands.Count -eq 0 -and $script:FullChecks -eq 1) $case
    } elseif ($case -eq 'low-memory') {
        Assert-Test ($code -eq 1 -and $stages.Count -eq 1 -and -not $started) $case
    } else {
        Assert-Test ($code -eq 1 -and $stages.Count -eq 0 -and -not $started -and -not $script:Followed) $case
        if ($case -eq 'no-device') { Assert-Test ($script:FullChecks -eq 0) 'No image processing before fastboot detection' }
    }
    if ($script:Session -and (Test-Path -LiteralPath $script:Session)) {
        [IO.File]::WriteAllText((Join-Path $script:Session 'SIMULATED-TEST.txt'),
            "Offline launcher test: $case. No real USB commands were sent.", $script:Utf8)
    }
    $script:TestCases += [pscustomobject]@{name=$case; passed=$true; commands=$commands}
    Write-Host "PASS: $case"
}
foreach ($arguments in @(@('flash','boot','bad.img'),@('erase','userdata'),@('reboot'),@('oem','stop'),
    @('get_staged',(Join-Path $script:Root 'outside-session.log')))) {
    $blocked = $false
    try { Assert-Command $arguments 'xnuloader' } catch { $blocked = $true }
    Assert-Test $blocked ('Disallowed command: ' + ($arguments -join ' '))
}
$script:TestCases += [pscustomobject]@{name='partition-stop-and-reboot-commands-blocked'; passed=$true}
$record = [ordered]@{windows_powershell=$PSVersionTable.PSVersion.ToString();
    device_access=$false; actual_apfs_and_xnup_checked=$true; cases=$script:TestCases}
Save-Json (Join-Path $script:RealRoot 'validation\launcher-tests.json') $record
Write-Host 'ALL LAUNCHER TESTS PASSED. No real device commands were issued.'
