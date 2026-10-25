[CmdletBinding()]
param(
    [ValidateSet('Start', 'Logs', 'Check', 'Verify')][string]$Mode = 'Start',
    [ValidateRange(0, 86400)][int]$ObserveSeconds = 0,
    [string]$LogDirectory = '',
    [switch]$ShowLog
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Root = $PSScriptRoot
$script:Session = $null
$script:Utf8 = New-Object System.Text.UTF8Encoding($false)
$script:Channels = @{}
$script:Steps = @()
$script:Fastboot = Join-Path $script:Root 'tools\platform-tools\fastboot.exe'

function Save-Json($Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 12), $script:Utf8)
}

function Get-PackagePath([string]$Relative) {
    if ([string]::IsNullOrWhiteSpace($Relative) -or [IO.Path]::IsPathRooted($Relative) -or $Relative.Contains(':')) {
        throw "Package path must be relative: $Relative"
    }
    $full = [IO.Path]::GetFullPath((Join-Path $script:Root $Relative))
    $prefix = [IO.Path]::GetFullPath($script:Root).TrimEnd('\') + '\'
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package path escapes its directory: $Relative"
    }
    return $full
}

function Test-Files([bool]$ForceHash, [bool]$ToolsOnly = $false) {
    $cachePath = Join-Path $script:Root 'logs\verified-files.json'
    $cache = @{}
    if (-not $ForceHash -and (Test-Path -LiteralPath $cachePath)) {
        try {
            foreach ($row in (Get-Content -LiteralPath $cachePath -Raw | ConvertFrom-Json)) {
                $cache[$row.path] = $row
            }
        } catch { $cache = @{} }
    }
    $verified = @()
    foreach ($item in $script:Package.files) {
        if ($ToolsOnly -and -not $item.path.StartsWith('tools/platform-tools/')) { continue }
        $path = Get-PackagePath $item.path
        $file = Get-Item -LiteralPath $path
        if ($file.Length -ne $item.bytes) { throw "File size mismatch: $($item.path)" }
        $known = $cache[$item.path]
        $hit = $null -ne $known -and $known.bytes -eq $file.Length -and
            $known.ticks -eq $file.LastWriteTimeUtc.Ticks -and $known.sha256 -eq $item.sha256
        if ($ForceHash -or -not $hit) {
            Write-Host "SHA-256: $($item.path) ($($file.Length) bytes)"
            $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -ne $item.sha256) { throw "Checksum mismatch: $($item.path)" }
        }
        $verified += [pscustomobject]@{path=$item.path; bytes=$file.Length;
            ticks=$file.LastWriteTimeUtc.Ticks; sha256=$item.sha256}
    }
    if (-not $ToolsOnly) { Save-Json $cachePath @($verified) }
}

function Assert-Apfs([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $header = New-Object byte[] 4096
        if ($stream.Read($header, 0, 4096) -ne 4096 -or
            [Text.Encoding]::ASCII.GetString($header, 32, 4) -ne 'NXSB') {
            throw "Invalid APFS container: $Path"
        }
        $length = [uint64][BitConverter]::ToUInt32($header, 36) * [BitConverter]::ToUInt64($header, 40)
        if ($length -ne $stream.Length) { throw "Incomplete APFS container: $Path" }
    } finally { $stream.Dispose() }
}

function Assert-Payload {
    $data = [IO.File]::ReadAllBytes((Get-PackagePath $script:Package.payload))
    if ($data.Length -lt 32 -or $data.Length -ge 0x03fbf000 -or
        [Text.Encoding]::ASCII.GetString($data, 0, 4) -ne 'XNUP' -or
        [BitConverter]::ToUInt32($data, 4) -ne $data.Length - 8) { throw 'Invalid XNUP payload' }
    $entries = @{}
    $pos = 8
    while ($pos + 24 -le $data.Length) {
        $name = [Text.Encoding]::ASCII.GetString($data, $pos, 16).TrimEnd([char]0)
        $size = [BitConverter]::ToUInt64($data, $pos + 16)
        $pos += 24
        if ($name -eq '') {
            if ($size -ne 0 -or $pos -ne $data.Length) { throw 'Invalid payload terminator' }
            break
        }
        if ($entries.ContainsKey($name) -or $size -gt $data.Length - $pos) { throw 'Invalid payload entry' }
        $bytes = New-Object byte[] ([int]$size)
        [Array]::Copy($data, $pos, $bytes, 0, [int]$size)
        $entries[$name] = $bytes
        $pos += [int]$size + ((16 - ([int]$size % 16)) % 16)
    }
    if ($pos -ne $data.Length -or $entries.Count -ne 4) { throw 'Unexpected payload structure' }
    foreach ($pair in @(@('kernel', $script:Package.kernel), @('ramdisk', $script:Package.boot))) {
        $hash = [Security.Cryptography.SHA256]::Create()
        try { $hex = [BitConverter]::ToString($hash.ComputeHash([byte[]]$entries[$pair[0]])).Replace('-', '').ToLowerInvariant() }
        finally { $hash.Dispose() }
        $expected = @($script:Package.files | Where-Object { $_.path -eq $pair[1] })
        if ($expected.Count -ne 1 -or $hex -ne $expected[0].sha256) { throw "Payload $($pair[0]) does not match exported image" }
    }
    $line = [Text.Encoding]::ASCII.GetString($entries['cmdline']).TrimEnd([char]0)
    if ($line -ne $script:Package.cmdline -or
        [Text.Encoding]::ASCII.GetString($entries['checkpoint']).TrimEnd([char]0) -ne '0') {
        throw 'Unexpected boot arguments or diagnostic checkpoint'
    }
}

function Assert-Command([string[]]$Arguments, [string]$Serial) {
    $joined = $Arguments -join ' '
    if ($joined -eq 'devices -l') { return }
    if (-not $Serial) { throw 'A target serial is required' }
    if ($Arguments.Count -eq 2 -and $Arguments[0] -eq 'getvar') { return }
    if ($Arguments.Count -eq 2 -and $Arguments[0] -eq 'boot' -and
        $Arguments[1] -eq (Get-PackagePath $script:Package.uefi)) { return }
    if ($Arguments.Count -eq 2 -and $Arguments[0] -eq 'stage' -and
        $Arguments[1] -in @((Get-PackagePath $script:Package.system), (Get-PackagePath $script:Package.payload))) { return }
    if ($joined -in @('oem battery-probe', 'oem ramdisk none', 'oem ramdisk', 'oem persist', 'oem follow 2')) { return }
    throw "Unsupported command: $joined"
}

function Invoke-Fastboot([string[]]$Arguments, [string]$Serial = '', [int]$TimeoutSeconds = 15) {
    Assert-Command $Arguments $Serial
    $allArgs = @()
    if ($Serial) { $allArgs += @('-s', $Serial) }
    $allArgs += $Arguments
    # Invoke the executable directly. Windows file names cannot contain quotes;
    # double trailing backslashes before the closing command-line quote.
    $quoted = foreach ($a in $allArgs) {
        if ($a.Contains('"') -or $a.Contains([char]0)) { throw 'Invalid argument' }
        '"' + ($a -replace '(\\+)$', '$1$1') + '"'
    }
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $script:Fastboot
    $psi.Arguments = $quoted -join ' '
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = $script:Utf8
    $psi.StandardErrorEncoding = $script:Utf8
    $proc = New-Object Diagnostics.Process
    $proc.StartInfo = $psi
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $lastProgress = 0
    $row = [ordered]@{utc=[DateTime]::UtcNow.ToString('o'); serial=$Serial; args=$Arguments}
    $started = $false
    try {
        if (-not $proc.Start()) { throw 'Cannot start fastboot' }
        $started = $true
        $stdout = $proc.StandardOutput.ReadToEndAsync()
        $stderr = $proc.StandardError.ReadToEndAsync()
        while (-not $proc.WaitForExit(200)) {
            if ($Arguments[0] -in @('stage', 'boot') -and $watch.Elapsed.TotalSeconds - $lastProgress -ge 15) {
                Write-Host ("{0} 仍在进行，已等待 {1:N0} 秒，请保持 USB 连接..." -f $Arguments[0], $watch.Elapsed.TotalSeconds)
                $lastProgress = $watch.Elapsed.TotalSeconds
            }
            if ($watch.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
                $proc.Kill()
                $null = $proc.WaitForExit(5000)
                $row.timeout = $true
                throw "USB command timed out ($TimeoutSeconds seconds). Keep the current phone state; check USB cable/WinUSB."
            }
        }
        $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        $row.exit_code = $proc.ExitCode
        $row.output = $output
        if ($proc.ExitCode -ne 0 -or $output -match '(?im)\bFAILED\b|^error:|^fastboot: error:') {
            throw "fastboot failed: $output"
        }
        return $output
    } finally {
        if ($started -and -not $proc.HasExited) { $proc.Kill(); $null = $proc.WaitForExit(5000) }
        $row.seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
        if ($script:Session) {
            [IO.File]::AppendAllText((Join-Path $script:Session 'commands.jsonl'),
                (($row | ConvertTo-Json -Depth 5 -Compress) + "`n"), $script:Utf8)
        }
        $proc.Dispose()
    }
}

function Get-VariableValue([string]$Name, [string]$Serial = 'xnuloader') {
    $answer = Invoke-Fastboot @('getvar', $Name) $Serial
    $pattern = '(?im)^(?:\(bootloader\)\s*)?' + [regex]::Escape($Name) + ':\s*([^\r\n]*)'
    $match = [regex]::Match($answer, $pattern)
    if (-not $match.Success) { throw "Missing fastboot variable: $Name" }
    return $match.Groups[1].Value.Trim()
}

function Get-Devices {
    $answer = Invoke-Fastboot @('devices', '-l')
    return @([regex]::Matches($answer, '(?m)^([^\s]+)\s+fastboot\b') | ForEach-Object { $_.Groups[1].Value })
}

function Assert-Loader {
    foreach ($pair in @(
        @('version-bootloader', 'dada-touch-spi-v16-mynewt-r16h'), @('platform', 'dada'),
        @('run-control', 'persistent-follow-v1'), @('log-protocol', 'DL1-base64-fastboot-INFO'),
        @('auto-return', 'disabled'), @('battery-service', 'dada-mynewt-bq27z561-read-v4'))) {
        $value = Get-VariableValue $pair[0]
        if ($value -ne $pair[1]) { throw "Wrong loader: $($pair[0])=$value; expected $($pair[1])" }
    }
}

function Receive-Log([string]$Output) {
    [IO.File]::AppendAllText((Join-Path $script:Session 'fastboot-stream.txt'), $Output, $script:Utf8)
    foreach ($line in ($Output -split '\r?\n')) {
        $match = [regex]::Match($line.Trim(), '^\(bootloader\)\s+DL1([FLKS]):([A-Za-z0-9+/]*={0,2})\s*$')
        if (-not $match.Success) { continue }
        $channel = $script:Channels[$match.Groups[1].Value]
        $bytes = [Convert]::FromBase64String($match.Groups[2].Value)
        $channel.stream.Write($bytes, 0, $bytes.Length)
        $channel.stream.Flush()
        $channel.count += $bytes.Length
        $chars = New-Object char[] ($script:Utf8.GetMaxCharCount($bytes.Length))
        $length = $channel.decoder.GetChars($bytes, 0, $bytes.Length, $chars, 0, $false)
        $text = if ($length) { [string]::new($chars, 0, $length) } else { '' }
        if ($ShowLog -and $text) {
            $safe = [regex]::Replace($text, '[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]', '')
            Write-Host -NoNewline $safe
        }
    }
}

function Follow-Logs {
    foreach ($pair in @(@('F', 'firmware'), @('L', 'loader'), @('K', 'kernel-live'), @('S', 'status'))) {
        $script:Channels[$pair[0]] = @{stream=[IO.File]::Open((Join-Path $script:Session ($pair[1] + '.log')),
            [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read);
            decoder=$script:Utf8.GetDecoder(); count=0L}
    }
    Write-Host "日志目录：$script:Session"
    Write-Host '正在采集日志。按 Q，或在日志目录创建 DETACH 文件，可结束电脑采集；手机继续运行。'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $last = -15
    try {
        while ($true) {
            $end = Test-Path -LiteralPath (Join-Path $script:Session 'DETACH')
            if (-not [Console]::IsInputRedirected -and [Console]::KeyAvailable) {
                if ([Console]::ReadKey($true).Key -eq 'Q') { $end = $true }
            }
            if ($end -or ($ObserveSeconds -gt 0 -and $watch.Elapsed.TotalSeconds -ge $ObserveSeconds)) { break }
            if ($watch.Elapsed.TotalSeconds - $last -ge 15) {
                $runtime = Get-VariableValue 'runtime'
                $heartbeat = Get-VariableValue 'kernel-heartbeat'
                $soc = Get-VariableValue 'battery-soc'
                if ($runtime -ne 'running') { throw "XNU has returned: runtime=$runtime; keep phone state and inspect saved logs" }
                $status = [ordered]@{utc=[DateTime]::UtcNow.ToString('o'); runtime=$runtime;
                    heartbeat=$heartbeat; battery_percent=$soc; kernel_log_bytes=$script:Channels['K'].count}
                Save-Json (Join-Path $script:Session 'status.json') $status
                Write-Host ("iOS 运行中 | heartbeat={0} | 电量={1}% | 已保存内核日志={2:N0} 字节" -f $heartbeat, $soc, $script:Channels['K'].count)
                $last = $watch.Elapsed.TotalSeconds
            }
            $output = Invoke-Fastboot @('oem', 'follow', '2') 'xnuloader' 45
            Receive-Log $output
        }
    } finally {
        foreach ($channel in $script:Channels.Values) { $channel.stream.Dispose() }
        Write-Host '电脑采集已结束，已保留日志。手机未被停止或重启。'
    }
}

function Start-Main {
    try {
        [Console]::OutputEncoding = $script:Utf8
        $manifest = Join-Path $script:Root 'package.json'
        $script:Package = Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json
        $sessionName = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
        $logBase = if ([string]::IsNullOrWhiteSpace($LogDirectory)) {
            Join-Path $script:Root 'logs'
        } else {
            [IO.Path]::GetFullPath($LogDirectory)
        }
        $script:Session = Join-Path $logBase $sessionName
        $null = New-Item -ItemType Directory -Path $script:Session -Force
        Write-Host 'hackios-dada-dev1.0 | iOS 16.5 (20F66) | r16h-wallpaper-r1'
        Write-Host '仅使用 RAM 临时启动；支持 dada，自动返回计时关闭。'
        if ($Mode -ne 'Verify') {
            Test-Files $false $true
            $initial = @(Get-Devices)
            if ($initial.Count -ne 1) {
                throw "Fastboot detection failed: found $($initial.Count) devices. Connect exactly one phone in stock FASTBOOT or the supported xnuloader; startup has not begun."
            }
            Write-Host "已检测到 fastboot 设备：$($initial[0])"
        }
        Test-Files ($Mode -eq 'Verify') ($Mode -in @('Logs', 'Check'))
        if ($Mode -notin @('Logs', 'Check')) {
            Assert-Apfs (Get-PackagePath $script:Package.system)
            Assert-Apfs (Get-PackagePath $script:Package.boot)
            Assert-Payload
        }
        if ($Mode -eq 'Verify') { Write-Host '全部镜像与工具的 SHA-256、APFS/XNUP 结构检查通过。'; return 0 }
        $devices = @(Get-Devices)
        if ($devices.Count -ne 1) { throw "Expected one fastboot device; found $($devices.Count). Enter stock FASTBOOT and check the Windows driver." }
        if ($devices[0] -ne 'xnuloader') {
            $serial = $devices[0]
            $product = Get-VariableValue 'product' $serial
            Write-Host "原厂 FASTBOOT：$serial，product=$product"
            if ($product -ne 'dada') { throw 'This package supports dada only' }
            if ($Mode -eq 'Check') { return 0 }
            if ($Mode -eq 'Logs') { throw 'iOS is not running; use Start-iOS.cmd to start it first' }
            Write-Host '[1/5] 正在临时启动 UEFI...'
            $answer = Invoke-Fastboot @('boot', (Get-PackagePath $script:Package.uefi)) $serial 180
            Write-Host $answer
            Write-Host '等待 xnuloader USB 设备。首次使用需为它安装 WinUSB；它与原厂 FASTBOOT 是两个设备。'
            $watch = [Diagnostics.Stopwatch]::StartNew()
            while ($true) {
                $ready = @(Get-Devices)
                if ($ready.Count -eq 1 -and $ready[0] -eq 'xnuloader') { break }
                if ($watch.Elapsed.TotalSeconds -ge 180) { throw 'xnuloader did not enumerate. Check the xnuloader WinUSB driver, then run the tool again; do not flash anything.' }
                Start-Sleep -Seconds 2
            }
        }
        Assert-Loader
        $runtime = Get-VariableValue 'runtime'
        Write-Host "加载器已连接，runtime=$runtime"
        if ($Mode -eq 'Check') {
            Write-Host ('电量=' + (Get-VariableValue 'battery-soc') + '%')
            return 0
        }
        if ($runtime -eq 'running') {
            Write-Host '手机已有 iOS 在运行，保留当前系统，仅接入日志。要启动本包，请先手动回到原厂 FASTBOOT。'
        } else {
            if ($runtime -ne 'stopped' -or $Mode -eq 'Logs') { throw 'Expected stopped loader for startup, or running iOS for log capture' }
            Write-Host '[2/5] 检查真实电池数据...'
            $null = Invoke-Fastboot @('oem', 'battery-probe') 'xnuloader' 90
            $batteryStatus = Get-VariableValue 'battery-status'
            $soc = Get-VariableValue 'battery-soc'
            if ($batteryStatus -ne '0' -or $soc -notmatch '^\d{1,3}$' -or [int]$soc -gt 100) {
                throw "Battery preflight failed: status=$batteryStatus, SOC=$soc. No XNU started."
            }
            Write-Host "电量 $soc%。[3/5] 上传 System 到 RAM，约 2.5 GB，可能需要数分钟..."
            $null = Invoke-Fastboot @('oem', 'ramdisk', 'none') 'xnuloader'
            $null = Invoke-Fastboot @('oem', 'ramdisk') 'xnuloader'
            $upload = Invoke-Fastboot @('stage', (Get-PackagePath $script:Package.system)) 'xnuloader' 900
            Write-Host $upload
            foreach ($name in @('xnu-memory-limit-mib', 'xnu-memory-mib')) {
                $size = Get-VariableValue $name
                if ($size -ne '8192') { throw "Firmware RAM check failed: $name=$size; no XNU started" }
            }
            Write-Host '[4/5] 上传 XNU 内核与 Boot APFS...'
            $payload = Invoke-Fastboot @('stage', (Get-PackagePath $script:Package.payload)) 'xnuloader' 120
            Write-Host $payload
            Write-Host '[5/5] 启动 iOS 16.5...'
            $null = Invoke-Fastboot @('oem', 'persist') 'xnuloader' 30
            if ((Get-VariableValue 'runtime') -ne 'running') { throw 'XNU did not enter running state; inspect logs' }
        }
        Follow-Logs
        return 0
    } catch {
        $message = $_.Exception.Message
        if ($script:Session) { [IO.File]::WriteAllText((Join-Path $script:Session 'error.txt'), ($_ | Out-String), $script:Utf8) }
        Write-Host "停止：$message" -ForegroundColor Red
        Write-Host "日志：$script:Session"
        Write-Host '没有执行刷写、清除分区或自动重启。'
        return 1
    }
}

if ($MyInvocation.InvocationName -ne '.') { exit (Start-Main) }
