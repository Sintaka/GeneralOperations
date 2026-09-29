# Shared first-run Python bootstrap for the Qt5 and Tauri2 frontends.
# PowerShell 5.1 compatible; all runtime state lives under GO_RUNTIME_HOME or
# %LOCALAPPDATA%/GeneralOperations/runtime.

$script:PythonInstallerUrl = 'https://www.python.org/ftp/python/3.13.15/python-3.13.15-amd64.exe'
$script:PythonInstallerSha256 = 'EDEC09C4853AEAE9AC36EFB8C9F95B6B8E2FEE65EEE56D9767A8B7C69C574403'

function Write-LauncherMessage {
    param([string]$Message, [switch]$ErrorMessage)
    if ($ErrorMessage) { [Console]::Error.WriteLine($Message) }
    else { [Console]::Out.WriteLine($Message) }
}

function ConvertTo-WindowsArgument {
    param([AllowEmptyString()][string]$Value)

    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append([char]'"')
    $slashes = 0
    for ($i = 0; $i -lt $Value.Length; $i++) {
        $ch = $Value[$i]
        if ($ch -eq [char]'\') {
            $slashes++
            continue
        }
        if ($ch -eq [char]'"') {
            if ($slashes -gt 0) { [void]$builder.Append([char]'\', (2 * $slashes)) }
            [void]$builder.Append([char]'\')
            [void]$builder.Append([char]'"')
        }
        else {
            if ($slashes -gt 0) { [void]$builder.Append([char]'\', $slashes) }
            [void]$builder.Append($ch)
        }
        $slashes = 0
    }
    if ($slashes -gt 0) { [void]$builder.Append([char]'\', (2 * $slashes)) }
    [void]$builder.Append([char]'"')
    return $builder.ToString()
}

function Invoke-ExternalProcess {
    param(
        [Parameter(Mandatory=$true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory,
        [switch]$ForwardOutput,
        [scriptblock]$ProcessRunner
    )

    if ($ProcessRunner) {
        $runnerArgs = @{
            FilePath = $FilePath
            Arguments = @($Arguments)
            WorkingDirectory = $WorkingDirectory
            ForwardOutput = [bool]$ForwardOutput
        }
        return (& $ProcessRunner @runnerArgs)
    }

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = (($Arguments | ForEach-Object { ConvertTo-WindowsArgument ([string]$_) }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $startInfo.StandardErrorEncoding = New-Object System.Text.UTF8Encoding($false)
    $startInfo.EnvironmentVariables['PYTHONUTF8'] = '1'
    $startInfo.EnvironmentVariables['PIP_NO_INPUT'] = '1'
    $startInfo.EnvironmentVariables['PIP_DISABLE_PIP_VERSION_CHECK'] = '1'
    # The caller may have disabled user site-packages or pointed Python at a
    # different installation. Both would invalidate the interpreter probe
    # immediately after pip --user succeeds.
    [void]$startInfo.EnvironmentVariables.Remove('PYTHONNOUSERSITE')
    [void]$startInfo.EnvironmentVariables.Remove('PYTHONPATH')
    [void]$startInfo.EnvironmentVariables.Remove('PYTHONHOME')
    if ($WorkingDirectory -and (Test-Path -LiteralPath $WorkingDirectory -PathType Container)) {
        $startInfo.WorkingDirectory = $WorkingDirectory
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    $stdout = New-Object 'System.Collections.Generic.List[string]'
    $stderr = New-Object 'System.Collections.Generic.List[string]'
    try {
        if (-not $process.Start()) {
            return [pscustomobject]@{ ExitCode = 1; Stdout = ''; Stderr = '无法启动进程: ' + $FilePath }
        }
        $outTask = $process.StandardOutput.ReadLineAsync()
        $errTask = $process.StandardError.ReadLineAsync()
        $outPending = $true
        $errPending = $true
        while ($outPending -or $errPending) {
            $tasks = New-Object 'System.Collections.Generic.List[System.Threading.Tasks.Task]'
            $channels = New-Object 'System.Collections.Generic.List[string]'
            if ($outPending) { $tasks.Add($outTask); $channels.Add('out') }
            if ($errPending) { $tasks.Add($errTask); $channels.Add('err') }
            $winner = [System.Threading.Tasks.Task]::WaitAny($tasks.ToArray())
            $channel = $channels[$winner]
            $lineTask = $tasks[$winner]
            $line = $lineTask.GetAwaiter().GetResult()
            if ($null -eq $line) {
                if ($channel -eq 'out') { $outPending = $false }
                else { $errPending = $false }
                continue
            }
            if ($channel -eq 'out') {
                $stdout.Add($line)
                if ($ForwardOutput) { [Console]::Out.WriteLine($line) }
                $outTask = $process.StandardOutput.ReadLineAsync()
            }
            else {
                $stderr.Add($line)
                if ($ForwardOutput) { [Console]::Error.WriteLine($line) }
                $errTask = $process.StandardError.ReadLineAsync()
            }
        }
        $process.WaitForExit()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = [string]::Join("`n", $stdout.ToArray())
            Stderr = [string]::Join("`n", $stderr.ToArray())
        }
    }
    catch {
        return [pscustomobject]@{ ExitCode = 1; Stdout = ''; Stderr = $_.Exception.Message }
    }
    finally {
        $process.Dispose()
    }
}

function Normalize-Requirements {
    param([object[]]$Requirements = @())

    $normalized = New-Object 'System.Collections.Generic.List[string]'
    $seen = @{}
    foreach ($raw in $Requirements) {
        if (-not ($raw -is [string])) {
            throw 'requirements 中的每个项目都必须是字符串。'
        }
        $token = $raw.Trim()
        $package = $token -replace '\?$', ''
        if (-not $package -or $package -notmatch '^[A-Za-z0-9_.-]+(?:==[A-Za-z0-9.*+!-]+)?$') {
            throw "@requires 项格式无效: $raw"
        }
        $key = $package.ToLowerInvariant()
        if (-not $seen.ContainsKey($key)) {
            $seen[$key] = $true
            $normalized.Add($package)
        }
    }
    return $normalized.ToArray()
}

function Get-PythonCandidates {
    param([scriptblock]$CandidateProvider, [scriptblock]$ProcessRunner)

    if ($CandidateProvider) { return @(& $CandidateProvider) }
    $candidates = New-Object 'System.Collections.Generic.List[string]'
    if ($env:GO_PYTHON) {
        $explicit = $env:GO_PYTHON.Trim().Trim('"')
        if (Test-Path -LiteralPath $explicit -PathType Leaf) { $candidates.Add($explicit) }
        else {
            $command = Get-Command -Name $explicit -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($command) { $candidates.Add($command.Source) }
            else { Write-LauncherMessage "GO_PYTHON 指向的解释器不可用: $explicit" -ErrorMessage }
        }
    }
    foreach ($name in @('python.exe', 'python3.exe')) {
        foreach ($command in (Get-Command -Name $name -CommandType Application -All -ErrorAction SilentlyContinue)) {
            if ($command.Source) { $candidates.Add($command.Source) }
        }
    }
    $py = Get-Command -Name 'py.exe' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($py) {
        $listed = Invoke-ExternalProcess -FilePath $py.Source -Arguments @('-0p') -ProcessRunner $ProcessRunner
        if ($listed.ExitCode -eq 0) {
            foreach ($line in ($listed.Stdout -split '\r?\n')) {
                if ($line -match '(?i)([A-Z]:\\.*?python(?:w)?\.exe)\s*$') { $candidates.Add($matches[1]) }
            }
        }
    }
    foreach ($root in @(
        'HKCU:\Software\Python\PythonCore',
        'HKLM:\Software\Python\PythonCore',
        'HKLM:\Software\WOW6432Node\Python\PythonCore'
    )) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        foreach ($versionKey in (Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
            $installPath = Join-Path $versionKey.PSPath 'InstallPath'
            if (-not (Test-Path -LiteralPath $installPath)) { continue }
            $entry = Get-Item -LiteralPath $installPath -ErrorAction SilentlyContinue
            if (-not $entry) { continue }
            $exe = $entry.GetValue('ExecutablePath')
            if (-not $exe) {
                $directory = $entry.GetValue('')
                if ($directory) { $exe = Join-Path $directory 'python.exe' }
            }
            if ($exe) { $candidates.Add([string]$exe) }
        }
    }

    $unique = New-Object 'System.Collections.Generic.List[string]'
    $seen = @{}
    foreach ($candidate in $candidates) {
        if (-not $candidate) { continue }
        try { $full = [System.IO.Path]::GetFullPath($candidate) }
        catch { $full = $candidate }
        if ($full -match '(?i)\\Microsoft\\WindowsApps\\') { continue }
        $key = $full.ToLowerInvariant()
        if (-not $seen.ContainsKey($key)) {
            $seen[$key] = $true
            $unique.Add($full)
        }
    }
    return $unique.ToArray()
}

function Invoke-PythonChecker {
    param(
        [Parameter(Mandatory=$true)][string]$Python,
        [Parameter(Mandatory=$true)][string[]]$Arguments,
        [scriptblock]$ProcessRunner
    )
    $checker = Join-Path $PSScriptRoot 'runtime-check.py'
    if (-not (Test-Path -LiteralPath $checker -PathType Leaf)) {
        return [pscustomobject]@{ Ok = $false; Problems = @('runtime-check.py 不存在') }
    }
    $result = Invoke-ExternalProcess -FilePath $Python -Arguments (@($checker) + $Arguments) -ProcessRunner $ProcessRunner
    $parsed = $null
    foreach ($line in ($result.Stdout -split '\r?\n')) {
        if ($line.TrimStart().StartsWith('{')) {
            try { $parsed = $line | ConvertFrom-Json -ErrorAction Stop } catch {}
        }
    }
    if ($null -eq $parsed) {
        $details = if ($result.Stderr) { $result.Stderr } else { $result.Stdout }
        return [pscustomobject]@{ Ok = $false; Problems = @("探测失败 (退出码 $($result.ExitCode)): $details") }
    }
    $problems = @()
    if ($parsed.problems) { $problems += @($parsed.problems) }
    if ($parsed.missing) { $problems += @($parsed.missing) }
    return [pscustomobject]@{ Ok = ([bool]$parsed.ok -and $result.ExitCode -eq 0); Problems = $problems }
}

function Test-PythonBase {
    param([string]$Python, [scriptblock]$ProcessRunner)
    $result = Invoke-PythonChecker -Python $Python -Arguments @('--base') -ProcessRunner $ProcessRunner
    if (-not $result.Ok) {
        Write-LauncherMessage "解释器不兼容 $Python：$($result.Problems -join '; ')" -ErrorMessage
        return $false
    }
    $pip = Invoke-ExternalProcess -FilePath $Python -Arguments @('-m', 'pip', '--version') -ProcessRunner $ProcessRunner
    if ($pip.ExitCode -ne 0) {
        $details = if ($pip.Stderr) { $pip.Stderr } else { $pip.Stdout }
        Write-LauncherMessage "解释器的 pip 无法运行 $Python：$details" -ErrorMessage
        return $false
    }
    return $true
}

function Test-PythonRequirements {
    param([string]$Python, [string[]]$Requirements, [scriptblock]$ProcessRunner)
    if (-not $Requirements -or $Requirements.Count -eq 0) { return $true }
    $json = ConvertTo-Json -InputObject @($Requirements) -Compress
    $result = Invoke-PythonChecker -Python $Python -Arguments @('--requirements-json', $json) -ProcessRunner $ProcessRunner
    if (-not $result.Ok) {
        Write-LauncherMessage "解释器缺少可导入依赖 $Python：$($result.Problems -join '; ')" -ErrorMessage
    }
    return [bool]$result.Ok
}

function Install-PythonRequirements {
    param(
        [string]$Python,
        [string[]]$Requirements,
        [switch]$UserScope,
        [scriptblock]$ProcessRunner
    )
    if (-not $Requirements -or $Requirements.Count -eq 0) { return $true }
    $baseArgs = @('-m', 'pip', 'install', '--disable-pip-version-check')
    if ($UserScope) { $baseArgs += '--user' }
    $dryRunArgs = $baseArgs + @('--dry-run', '--ignore-installed', '--only-binary=:all:') + $Requirements
    Write-LauncherMessage "正在检查全部依赖的兼容 wheel：$Python"
    $dryRun = Invoke-ExternalProcess -FilePath $Python -Arguments $dryRunArgs -ForwardOutput -ProcessRunner $ProcessRunner
    if ($dryRun.ExitCode -ne 0) {
        Write-LauncherMessage "依赖 wheel 检查失败 (退出码 $($dryRun.ExitCode))。" -ErrorMessage
        return $false
    }
    $installArgs = $baseArgs + @('--only-binary=:all:') + $Requirements
    Write-LauncherMessage "正在安装 Python 依赖：$Python"
    $installed = Invoke-ExternalProcess -FilePath $Python -Arguments $installArgs -ForwardOutput -ProcessRunner $ProcessRunner
    if ($installed.ExitCode -ne 0) {
        Write-LauncherMessage "依赖安装失败 (退出码 $($installed.ExitCode))。" -ErrorMessage
        return $false
    }
    return $true
}

function Get-ShortPathHash {
    param([string]$Value)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Value.ToLowerInvariant())
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').Substring(0, 12).ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function New-OrGetVenv {
    param([string]$BasePython, [string]$VenvDirectory, [scriptblock]$ProcessRunner)

    $venvPython = Join-Path $VenvDirectory 'Scripts\python.exe'
    if (Test-Path -LiteralPath $venvPython -PathType Leaf) {
        if (Test-PythonBase -Python $venvPython -ProcessRunner $ProcessRunner) { return $venvPython }
        Write-LauncherMessage "已有 venv 不可用，将重建: $VenvDirectory" -ErrorMessage
    }
    if (Test-Path -LiteralPath $VenvDirectory -PathType Container) {
        $createArgs = @('-m', 'venv', '--clear', $VenvDirectory)
    }
    else {
        $createArgs = @('-m', 'venv', $VenvDirectory)
    }
    Write-LauncherMessage "正在创建独立 venv：$VenvDirectory"
    $created = Invoke-ExternalProcess -FilePath $BasePython -Arguments $createArgs -ForwardOutput -ProcessRunner $ProcessRunner
    if ($created.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $venvPython -PathType Leaf)) {
        Write-LauncherMessage "创建 venv 失败 (退出码 $($created.ExitCode))。" -ErrorMessage
        return $null
    }
    if (-not (Test-PythonBase -Python $venvPython -ProcessRunner $ProcessRunner)) { return $null }
    return $venvPython
}

function Get-RuntimeHome {
    if ($env:GO_RUNTIME_HOME) { return [System.IO.Path]::GetFullPath($env:GO_RUNTIME_HOME) }
    $localAppData = $env:LOCALAPPDATA
    if (-not $localAppData) { $localAppData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData) }
    if (-not $localAppData) { throw '无法解析 LOCALAPPDATA，请设置 GO_RUNTIME_HOME。' }
    return [System.IO.Path]::GetFullPath((Join-Path $localAppData 'GeneralOperations\runtime'))
}

function Get-VerifiedPythonInstaller {
    param(
        [string]$RuntimeHome,
        [string]$ExpectedSha256 = $script:PythonInstallerSha256,
        [scriptblock]$Downloader
    )
    $downloadDir = Join-Path $RuntimeHome 'downloads'
    $installer = Join-Path $downloadDir 'python-3.13.15-amd64.exe'
    [void](New-Item -ItemType Directory -Path $downloadDir -Force)
    if (Test-Path -LiteralPath $installer -PathType Leaf) {
        $existingHash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash
        if ($existingHash -eq $ExpectedSha256) { return $installer }
        Remove-Item -LiteralPath $installer -Force
    }
    try {
        if ($Downloader) {
            $downloadArgs = @{ Uri = $script:PythonInstallerUrl; OutPath = $installer }
            & $Downloader @downloadArgs
        }
        else {
            try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
            Invoke-WebRequest -UseBasicParsing -Uri $script:PythonInstallerUrl -OutFile $installer -TimeoutSec 180 -ErrorAction Stop
        }
    }
    catch {
        if (Test-Path -LiteralPath $installer -PathType Leaf) { Remove-Item -LiteralPath $installer -Force }
        throw "下载官方 Python 安装器失败：$($_.Exception.Message)；请检查网络/TLS 后重试。"
    }
    if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) { throw "下载器没有生成安装器文件: $installer" }
    $actualHash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash
    if ($actualHash -ne $ExpectedSha256) {
        Remove-Item -LiteralPath $installer -Force
        throw "官方 Python 安装器 SHA256 不匹配，已删除下载文件。期望 $ExpectedSha256，实际 $actualHash"
    }
    return $installer
}

function Resolve-Runtime {
    param(
        [string[]]$Requirements,
        [string]$RuntimeHome,
        [scriptblock]$CandidateProvider,
        [scriptblock]$ProcessRunner,
        [scriptblock]$Downloader,
        [string]$ExpectedInstallerSha256 = $script:PythonInstallerSha256
    )

    $Requirements = @(Normalize-Requirements -Requirements @($Requirements))
    [void](New-Item -ItemType Directory -Path $RuntimeHome -Force)
    $candidates = @(Get-PythonCandidates -CandidateProvider $CandidateProvider -ProcessRunner $ProcessRunner)
    $compatible = New-Object 'System.Collections.Generic.List[string]'
    foreach ($candidate in $candidates) {
        if (-not $candidate) { continue }
        if (-not (Test-PythonBase -Python $candidate -ProcessRunner $ProcessRunner)) { continue }
        $compatible.Add($candidate)
        if (Test-PythonRequirements -Python $candidate -Requirements $Requirements -ProcessRunner $ProcessRunner) {
            Write-LauncherMessage "使用已兼容的系统 Python：$candidate"
            return $candidate
        }
        if ((Install-PythonRequirements -Python $candidate -Requirements $Requirements -UserScope -ProcessRunner $ProcessRunner) -and
            (Test-PythonRequirements -Python $candidate -Requirements $Requirements -ProcessRunner $ProcessRunner)) {
            Write-LauncherMessage "系统 Python 依赖已就绪：$candidate"
            return $candidate
        }

        $venvDir = Join-Path (Join-Path $RuntimeHome 'venvs') ('system-' + (Get-ShortPathHash $candidate))
        $venvPython = New-OrGetVenv -BasePython $candidate -VenvDirectory $venvDir -ProcessRunner $ProcessRunner
        if ($venvPython) {
            if (Test-PythonRequirements -Python $venvPython -Requirements $Requirements -ProcessRunner $ProcessRunner) {
                Write-LauncherMessage "使用兼容系统 Python 创建的 venv：$venvPython"
                return $venvPython
            }
            if ((Install-PythonRequirements -Python $venvPython -Requirements $Requirements -ProcessRunner $ProcessRunner) -and
                (Test-PythonRequirements -Python $venvPython -Requirements $Requirements -ProcessRunner $ProcessRunner)) {
                Write-LauncherMessage "venv 依赖已就绪：$venvPython"
                return $venvPython
            }
        }
    }

    $pythonHome = Join-Path $RuntimeHome 'python-3.13.15'
    $installedPython = Join-Path $pythonHome 'python.exe'
    $officialBaseReady = $false
    if (Test-Path -LiteralPath $installedPython -PathType Leaf) {
        $officialBaseReady = Test-PythonBase -Python $installedPython -ProcessRunner $ProcessRunner
    }
    if (-not $officialBaseReady) {
        try {
            $installer = Get-VerifiedPythonInstaller -RuntimeHome $RuntimeHome -ExpectedSha256 $ExpectedInstallerSha256 -Downloader $Downloader
        }
        catch {
            Write-LauncherMessage $_.Exception.Message -ErrorMessage
            return $null
        }
        [void](New-Item -ItemType Directory -Path $pythonHome -Force)
        # CPython's documented per-user, silent installer switches:
        # https://docs.python.org/3.13/using/windows.html#installing-without-ui
        $installerArgs = @(
            '/quiet', 'InstallAllUsers=0', ('TargetDir=' + $pythonHome),
            'PrependPath=0', 'Include_test=0', 'Include_launcher=0', 'Include_pip=1',
            'Include_doc=0', 'Include_tcltk=0', 'Include_symbols=0', 'Shortcuts=0'
        )
        Write-LauncherMessage "正在将 Python 3.13.15 安装到用户运行时目录：$pythonHome"
        $installed = Invoke-ExternalProcess -FilePath $installer -Arguments $installerArgs -ForwardOutput -ProcessRunner $ProcessRunner
        if ($installed.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $installedPython -PathType Leaf)) {
            Write-LauncherMessage "Python 安装失败 (退出码 $($installed.ExitCode))。" -ErrorMessage
            return $null
        }
        if (-not (Test-PythonBase -Python $installedPython -ProcessRunner $ProcessRunner)) { return $null }
    }

    $officialVenvDir = Join-Path (Join-Path $RuntimeHome 'venvs') 'python-3.13.15'
    $officialVenv = New-OrGetVenv -BasePython $installedPython -VenvDirectory $officialVenvDir -ProcessRunner $ProcessRunner
    if (-not $officialVenv) { return $null }
    if (-not (Test-PythonRequirements -Python $officialVenv -Requirements $Requirements -ProcessRunner $ProcessRunner)) {
        if (-not (Install-PythonRequirements -Python $officialVenv -Requirements $Requirements -ProcessRunner $ProcessRunner)) { return $null }
        if (-not (Test-PythonRequirements -Python $officialVenv -Requirements $Requirements -ProcessRunner $ProcessRunner)) { return $null }
    }
    Write-LauncherMessage "官方 Python venv 已就绪：$officialVenv"
    return $officialVenv
}

function Read-LauncherRequest {
    param([System.IO.TextReader]$InputReader)
    [Console]::InputEncoding = New-Object System.Text.UTF8Encoding($false)
    if ($InputReader) { $json = $InputReader.ReadToEnd() }
    else { $json = [Console]::In.ReadToEnd() }
    if ([string]::IsNullOrWhiteSpace($json)) { throw 'stdin 中没有启动请求 JSON。' }
    $json = $json.TrimStart([char]0xFEFF)
    $request = $json | ConvertFrom-Json -ErrorAction Stop
    foreach ($name in @('script', 'scriptsDir', 'args', 'requirements')) {
        if ($null -eq $request.PSObject.Properties[$name]) { throw "启动请求缺少字段: $name" }
    }
    if (-not ($request.script -is [string]) -or -not ($request.scriptsDir -is [string]) -or
        -not ($request.args -is [System.Array]) -or -not ($request.requirements -is [System.Array])) {
        throw '启动请求字段类型错误，应为 script/string、scriptsDir/string、args/array、requirements/array。'
    }
    foreach ($requirement in $request.requirements) {
        if (-not ($requirement -is [string])) {
            throw '启动请求字段类型错误，requirements 必须是字符串数组。'
        }
    }
    if (-not [System.IO.Path]::IsPathRooted($request.script) -or -not [System.IO.Path]::IsPathRooted($request.scriptsDir)) {
        throw 'script 与 scriptsDir 必须是绝对路径。'
    }
    $request.script = [System.IO.Path]::GetFullPath($request.script)
    $request.scriptsDir = [System.IO.Path]::GetFullPath($request.scriptsDir)
    if (-not (Test-Path -LiteralPath $request.script -PathType Leaf)) { throw "Python 脚本不存在: $($request.script)" }
    return $request
}

function Invoke-PythonLauncherMain {
    try {
        [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
        $request = Read-LauncherRequest
        $runtimeHome = Get-RuntimeHome
        $python = Resolve-Runtime -Requirements $request.requirements -RuntimeHome $runtimeHome
        if (-not $python) {
            Write-LauncherMessage 'Python 运行环境准备失败，脚本未启动。' -ErrorMessage
            return 1
        }
        $arguments = @('-u', $request.script) + @($request.args | ForEach-Object { [string]$_ })
        Write-LauncherMessage "Python 环境已就绪，开始执行脚本：$($request.script)"
        $result = Invoke-ExternalProcess -FilePath $python -Arguments $arguments -WorkingDirectory ([Environment]::CurrentDirectory) -ForwardOutput
        return [int]$result.ExitCode
    }
    catch {
        Write-LauncherMessage "Python 启动准备失败：$($_.Exception.Message)" -ErrorMessage
        return 1
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    exit (Invoke-PythonLauncherMain)
}
