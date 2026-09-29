$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'python-launcher.ps1')

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "ASSERT: $Message" }
}

function New-ProcessResult {
    param([int]$ExitCode = 0, [string]$Stdout = '', [string]$Stderr = '')
    return [pscustomobject]@{ ExitCode = $ExitCode; Stdout = $Stdout; Stderr = $Stderr }
}

function Test-ProcessCaptureAndExitCode {
    $childCode = '[Console]::Out.WriteLine("stdout-line"); [Console]::Error.WriteLine("stderr-line"); exit 23'
    $powershellExe = Join-Path $PSHOME 'powershell.exe'
    $result = Invoke-ExternalProcess -FilePath $powershellExe `
        -Arguments @('-NoProfile', '-NonInteractive', '-Command', $childCode)
    Assert-True ($result.ExitCode -eq 23) 'child exit code must be returned unchanged'
    Assert-True ($result.Stdout -eq 'stdout-line') 'stdout lines must be captured independently'
    Assert-True ($result.Stderr -eq 'stderr-line') 'stderr lines must be captured independently'
}

function Test-UnicodeLauncherRequest {
    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('go-runtime-unicode-' + [Guid]::NewGuid().ToString('N'))
    $scriptsDir = Join-Path $tempRoot '脚本 根目录'
    $scriptFile = Join-Path $scriptsDir '图像 脚本.py'
    [void](New-Item -ItemType Directory -Path $scriptsDir -Force)
    [System.IO.File]::WriteAllText($scriptFile, '# test', [System.Text.Encoding]::UTF8)
    $payload = @{
        script = $scriptFile
        scriptsDir = $scriptsDir
        args = @('--title', '雪景 图片', '--', 'D:\素材 路径\猫.exr')
        requirements = @('Pillow?', 'OpenEXR==3.2.0?')
    } | ConvertTo-Json -Compress
    try {
        $reader = New-Object System.IO.StringReader($payload)
        $request = Read-LauncherRequest -InputReader $reader
        Assert-True ($request.script -eq $scriptFile) 'Unicode script paths must survive JSON decoding'
        Assert-True ($request.scriptsDir -eq $scriptsDir) 'Unicode scriptsDir must survive JSON decoding'
        Assert-True ($request.args.Count -eq 4 -and $request.args[1] -eq '雪景 图片') 'argument strings and spaces must be preserved'
        Assert-True ($request.args[3] -eq 'D:\素材 路径\猫.exr') 'Unicode file arguments must be preserved'
        Assert-True ($request.requirements.Count -eq 2 -and $request.requirements[0] -eq 'Pillow?') 'raw requirements must survive JSON decoding'
    }
    finally {
        $resolvedRoot = [System.IO.Path]::GetFullPath($tempRoot)
        $tempPrefix = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolvedRoot.StartsWith($tempPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
        }
    }
}

function Test-LauncherRequirementsRequestValidation {
    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('go-runtime-request-' + [Guid]::NewGuid().ToString('N'))
    $scriptFile = Join-Path $tempRoot 'script.py'
    [void](New-Item -ItemType Directory -Path $tempRoot -Force)
    [System.IO.File]::WriteAllText($scriptFile, '# test', [System.Text.Encoding]::UTF8)
    $base = @{
        script = $scriptFile
        scriptsDir = $tempRoot
        args = @()
        requirements = @('Pillow?')
    }
    try {
        $missing = @{} + $base
        [void]$missing.Remove('requirements')
        $scalar = @{} + $base
        $scalar.requirements = 'Pillow?'
        $mixed = @{} + $base
        $mixed.requirements = @('Pillow?', 42)
        $invalidRequests = @($missing, $scalar, $mixed)
        foreach ($invalidRequest in $invalidRequests) {
            $rejected = $false
            try {
                $reader = New-Object System.IO.StringReader(($invalidRequest | ConvertTo-Json -Depth 5 -Compress))
                [void](Read-LauncherRequest -InputReader $reader)
            }
            catch { $rejected = $true }
            Assert-True $rejected 'requirements must be present as an array containing only strings'
        }
    }
    finally {
        $resolvedRoot = [System.IO.Path]::GetFullPath($tempRoot)
        $tempPrefix = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolvedRoot.StartsWith($tempPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
        }
    }
}

function Test-RequirementNormalization {
    $normalized = @(Normalize-Requirements -Requirements @('Pillow?', 'OpenEXR==3.2.0?', 'PILLOW', 'NumPy'))
    Assert-True ($normalized.Count -eq 3) 'normalization must remove the optional suffix and deduplicate case-insensitively'
    Assert-True ($normalized[0] -eq 'Pillow' -and $normalized[1] -eq 'OpenEXR==3.2.0') 'normalization must retain package spelling and pins'
    Assert-True ($normalized[2] -eq 'NumPy') 'normalization must preserve first-seen order'

    foreach ($invalid in @('bad/name', 'numpy??', '')) {
        $rejected = $false
        try { [void](Normalize-Requirements -Requirements @($invalid)) }
        catch { $rejected = $true }
        Assert-True $rejected "invalid requirement must be rejected: '$invalid'"
    }
    $rejectedNonString = $false
    try { [void](Normalize-Requirements -Requirements @('Pillow', $null)) }
    catch { $rejectedNonString = $true }
    Assert-True $rejectedNonString 'normalization must reject non-string requirement entries'
}

function Test-CompatibleSystemPython {
    $candidate = 'C:\mock\python.exe'
    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('go-runtime-compatible-' + [Guid]::NewGuid().ToString('N'))
    $state = [pscustomobject]@{ Requirements = @() }
    $runner = {
        param($FilePath, [string[]]$Arguments, $WorkingDirectory, [bool]$ForwardOutput)
        if ($Arguments.Count -eq 3 -and $Arguments[0] -eq '-m' -and $Arguments[1] -eq 'pip') { return (New-ProcessResult -Stdout 'pip 24.0') }
        if ($Arguments.Count -ge 2 -and $Arguments[1] -eq '--base') {
            return (New-ProcessResult -Stdout '{"ok":true,"problems":[]}')
        }
        if ($Arguments.Count -ge 2 -and $Arguments[1] -eq '--requirements-json') {
            $state.Requirements = @($Arguments[2] | ConvertFrom-Json)
            return (New-ProcessResult -Stdout '{"ok":true,"missing":[]}')
        }
        throw "Unexpected process call: $FilePath $($Arguments -join ' ')"
    }
    try {
        $resolved = Resolve-Runtime -Requirements @('Pillow?', 'pillow') -RuntimeHome $tempRoot `
            -CandidateProvider { return @($candidate) } -ProcessRunner $runner
        Assert-True ($resolved -eq $candidate) 'already compatible system Python should be selected directly'
        Assert-True ($state.Requirements.Count -eq 1 -and $state.Requirements[0] -eq 'Pillow') 'runtime checks must receive normalized catalog requirements'
    }
    finally {
        $resolvedRoot = [System.IO.Path]::GetFullPath($tempRoot)
        $tempPrefix = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolvedRoot.StartsWith($tempPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
        }
    }
}

function Test-InstallerHashMismatchIsRejected {
    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('go-runtime-bad-hash-' + [Guid]::NewGuid().ToString('N'))
    [void](New-Item -ItemType Directory -Path $tempRoot -Force)
    $downloadedFile = Join-Path $tempRoot 'downloads\python-3.13.15-amd64.exe'
    try {
        $rejected = $false
        try {
            [void](Get-VerifiedPythonInstaller -RuntimeHome $tempRoot -ExpectedSha256 ('0' * 64) -Downloader {
                param([string]$Uri, [string]$OutPath)
                [System.IO.File]::WriteAllText($OutPath, 'not the official installer')
            })
        }
        catch { $rejected = $true }
        Assert-True $rejected 'a non-matching installer hash must be rejected'
        Assert-True (-not (Test-Path -LiteralPath $downloadedFile)) 'a mismatched installer must be deleted'
    }
    finally {
        $resolvedRoot = [System.IO.Path]::GetFullPath($tempRoot)
        $tempPrefix = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolvedRoot.StartsWith($tempPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
        }
    }
}

function Test-SystemInstallFailureUsesVenv {
    $candidate = 'C:\mock\system-python.exe'
    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('go-runtime-venv-' + [Guid]::NewGuid().ToString('N'))
    $runtimeHome = Join-Path $tempRoot 'runtime'
    $venvDir = Join-Path (Join-Path $runtimeHome 'venvs') ('system-' + (Get-ShortPathHash $candidate))
    $venvPython = Join-Path $venvDir 'Scripts\python.exe'
    $state = [pscustomobject]@{ PackagesInstalled = $false }
    $runner = {
        param($FilePath, [string[]]$Arguments, $WorkingDirectory, [bool]$ForwardOutput)
        if ($Arguments.Count -eq 3 -and $Arguments[0] -eq '-m' -and $Arguments[1] -eq 'pip') { return (New-ProcessResult -Stdout 'pip 24.0') }
        if ($Arguments.Count -ge 2 -and $Arguments[0] -like '*runtime-check.py' -and $Arguments[1] -eq '--base') {
            return (New-ProcessResult -Stdout '{"ok":true,"problems":[]}')
        }
        if ($Arguments.Count -ge 2 -and $Arguments[0] -like '*runtime-check.py' -and $Arguments[1] -eq '--requirements-json') {
            if ($FilePath -eq $venvPython -and $state.PackagesInstalled) {
                return (New-ProcessResult -Stdout '{"ok":true,"missing":[]}')
            }
            return (New-ProcessResult -ExitCode 1 -Stdout '{"ok":false,"missing":["Pillow"]}')
        }
        if ($Arguments -contains '--dry-run') { return (New-ProcessResult) }
        if ($Arguments -contains '--only-binary=:all:') {
            if ($FilePath -eq $candidate) { return (New-ProcessResult -ExitCode 1 -Stderr 'mock system install failed') }
            $state.PackagesInstalled = $true
            return (New-ProcessResult)
        }
        if ($Arguments -contains 'venv') {
            [void](New-Item -ItemType Directory -Path (Split-Path $venvPython -Parent) -Force)
            [System.IO.File]::WriteAllText($venvPython, 'mock interpreter')
            return (New-ProcessResult)
        }
        throw "Unexpected process call: $FilePath $($Arguments -join ' ')"
    }
    $downloadState = [pscustomobject]@{ Called = $false }
    try {
        $resolved = Resolve-Runtime -Requirements @('Pillow') -RuntimeHome $runtimeHome `
            -CandidateProvider { return @($candidate) } -ProcessRunner $runner -Downloader { $downloadState.Called = $true }
        Assert-True ($resolved -eq $venvPython) 'a failed system install should fall back to a venv on that compatible interpreter'
        Assert-True (-not $downloadState.Called) 'official installer should not be used when a system-based venv works'
    }
    finally {
        $resolvedRoot = [System.IO.Path]::GetFullPath($tempRoot)
        $tempPrefix = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolvedRoot.StartsWith($tempPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
        }
    }
}

function Test-UnavailableSystemUsesVerifiedOfficialFallback {
    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('go-runtime-fallback-' + [Guid]::NewGuid().ToString('N'))
    $runtimeHome = Join-Path $tempRoot 'runtime'
    [void](New-Item -ItemType Directory -Path $tempRoot -Force)
    $pythonHome = Join-Path $runtimeHome 'python-3.13.15'
    $officialPython = Join-Path $pythonHome 'python.exe'
    $venvDir = Join-Path (Join-Path $runtimeHome 'venvs') 'python-3.13.15'
    $venvPython = Join-Path $venvDir 'Scripts\python.exe'
    $installerPath = Join-Path $runtimeHome 'downloads\python-3.13.15-amd64.exe'
    $payload = 'deterministic fake installer'
    $expectedHash = [BitConverter]::ToString([System.Security.Cryptography.SHA256]::Create().ComputeHash([System.Text.Encoding]::UTF8.GetBytes($payload))).Replace('-', '')
    $state = [pscustomobject]@{ PackagesInstalled = $false }
    $runner = {
        param($FilePath, [string[]]$Arguments, $WorkingDirectory, [bool]$ForwardOutput)
        if ($Arguments.Count -eq 3 -and $Arguments[0] -eq '-m' -and $Arguments[1] -eq 'pip') { return (New-ProcessResult -Stdout 'pip 24.0') }
        if ($FilePath -eq 'C:\mock\incompatible-python.exe' -and $Arguments.Count -ge 2 -and $Arguments[0] -like '*runtime-check.py') {
            return (New-ProcessResult -ExitCode 2 -Stdout '{"ok":false,"problems":["解释器不是 64 位"]}')
        }
        if ($Arguments.Count -ge 2 -and $Arguments[0] -like '*runtime-check.py' -and $Arguments[1] -eq '--base') {
            return (New-ProcessResult -Stdout '{"ok":true,"problems":[]}')
        }
        if ($Arguments.Count -ge 2 -and $Arguments[0] -like '*runtime-check.py' -and $Arguments[1] -eq '--requirements-json') {
            if ($FilePath -eq $venvPython -and $state.PackagesInstalled) {
                return (New-ProcessResult -Stdout '{"ok":true,"missing":[]}')
            }
            return (New-ProcessResult -ExitCode 1 -Stdout '{"ok":false,"missing":["Pillow"]}')
        }
        if ($FilePath -eq $installerPath) {
            [void](New-Item -ItemType Directory -Path $pythonHome -Force)
            [System.IO.File]::WriteAllText($officialPython, 'mock interpreter')
            return (New-ProcessResult)
        }
        if ($Arguments -contains 'venv') {
            [void](New-Item -ItemType Directory -Path (Split-Path $venvPython -Parent) -Force)
            [System.IO.File]::WriteAllText($venvPython, 'mock interpreter')
            return (New-ProcessResult)
        }
        if ($Arguments -contains '--dry-run') { return (New-ProcessResult) }
        if ($Arguments -contains '--only-binary=:all:') {
            $state.PackagesInstalled = $true
            return (New-ProcessResult)
        }
        throw "Unexpected process call: $FilePath $($Arguments -join ' ')"
    }
    $downloader = {
        param([string]$Uri, [string]$OutPath)
        Assert-True ($Uri -like 'https://www.python.org/ftp/python/3.13.15/*') 'official release URL should be used'
        [System.IO.File]::WriteAllText($OutPath, $payload)
    }
    try {
        $resolved = Resolve-Runtime -Requirements @('Pillow') -RuntimeHome $runtimeHome `
            -CandidateProvider { return @('C:\mock\incompatible-python.exe') } -ProcessRunner $runner `
            -Downloader $downloader -ExpectedInstallerSha256 $expectedHash
        Assert-True ($resolved -eq $venvPython) 'verified official install should create a ready venv when no system Python is compatible'
        Assert-True (Test-Path -LiteralPath (Join-Path $runtimeHome 'downloads\python-3.13.15-amd64.exe')) 'verified installer should be cached under runtimeHome'
    }
    finally {
        $resolvedRoot = [System.IO.Path]::GetFullPath($tempRoot)
        $tempPrefix = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolvedRoot.StartsWith($tempPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
        }
    }
}

Test-ProcessCaptureAndExitCode
Test-CompatibleSystemPython
Test-InstallerHashMismatchIsRejected
Test-SystemInstallFailureUsesVenv
Test-UnavailableSystemUsesVerifiedOfficialFallback
Test-UnicodeLauncherRequest
Test-LauncherRequirementsRequestValidation
Test-RequirementNormalization
Write-Output 'runtime bootstrap tests passed'
