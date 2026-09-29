# 构建 Tauri 2 便携发行包。所有 core 资源来自仓库现有实现。
param()

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tauriRoot = Join-Path $repoRoot 'ui/tauri2'
$outputRoot = Join-Path $repoRoot 'output'
$coreBuild = Join-Path $repoRoot 'build/tauri2-core-release'
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'build')).TrimEnd('\', '/')
$buildPathsJson = & node (Join-Path $tauriRoot 'scripts/build-env.mjs')
if ($LASTEXITCODE -ne 0) { throw '无法读取 Tauri 共享构建环境路径' }
$buildPaths = ($buildPathsJson -join "`n") | ConvertFrom-Json
foreach ($name in @('cargoHome', 'cargoTarget', 'npmCache')) {
    $candidate = [System.IO.Path]::GetFullPath([string]$buildPaths.$name)
    if (-not $candidate.StartsWith($buildRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Tauri 构建路径越过 build：$name=$candidate"
    }
}
$env:CARGO_HOME = $buildPaths.cargoHome
$env:CARGO_TARGET_DIR = $buildPaths.cargoTarget
$env:npm_config_cache = $buildPaths.npmCache

function Invoke-Checked([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command 失败，退出码 $LASTEXITCODE"
    }
}

$compiler = (Get-Command g++.exe -ErrorAction Stop).Source
$ninja = (Get-Command ninja.exe -ErrorAction Stop).Source

Invoke-Checked 'cmake' @(
    '-S', $repoRoot, '-B', $coreBuild, '-G', 'Ninja',
    '-DGO_FRONTEND=tauri2', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_CXX_COMPILER=$compiler", "-DCMAKE_MAKE_PROGRAM=$ninja",
    '-DCMAKE_EXE_LINKER_FLAGS=-static'
)

Push-Location $tauriRoot
try {
    Invoke-Checked 'npm.cmd' @('ci')
    Invoke-Checked 'npm.cmd' @('run', 'build')
}
finally { Pop-Location }

$releasePaths = Get-Content -LiteralPath (Join-Path $coreBuild 'go_release_paths.json') -Raw | ConvertFrom-Json
$bundleDir = Join-Path $releasePaths.bundle_dir 'Release'
$zip = $releasePaths.zip_file
if (-not $releasePaths.bundle_dir -or -not $zip -or -not $releasePaths.runtime_dir) {
    throw '根 CMake 未导出 Tauri 发行路径或 Python 运行时工具目录'
}
Invoke-Checked 'cmake' @('--build', $coreBuild, '--target', 'go_pmx2glb', 'go_script_catalog')

$tauriExe = Join-Path $env:CARGO_TARGET_DIR 'release/general-operations-tauri.exe'
$backendExe = Join-Path $coreBuild 'core/cpp/go_pmx2glb.exe'
$catalogExe = Join-Path $coreBuild 'core/cpp/go_script_catalog.exe'
foreach ($path in @($tauriExe, $backendExe, $catalogExe)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "构建产物不存在：$path" }
}

# 只清理本前端的装配目标，避免上次发行留下已删除的脚本或工具。
$resolvedOutput = [System.IO.Path]::GetFullPath($outputRoot).TrimEnd('\', '/')
$resolvedBundle = [System.IO.Path]::GetFullPath($bundleDir)
$resolvedZip = [System.IO.Path]::GetFullPath($zip)
if (-not $resolvedBundle.StartsWith($resolvedOutput + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "装配目录越过 output：$resolvedBundle"
}
if (-not $resolvedZip.StartsWith($resolvedOutput + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) { throw "zip 路径越过 output：$resolvedZip" }
if (Test-Path -LiteralPath $resolvedBundle) {
    foreach ($path in @($resolvedOutput, (Split-Path -Parent $resolvedBundle), $resolvedBundle)) {
        $item = Get-Item -LiteralPath $path -Force
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "装配路径包含重解析点，拒绝递归清理：$path"
        }
    }
    Remove-Item -LiteralPath $resolvedBundle -Recurse -Force
}

Invoke-Checked 'cmake' @(
    "-DBUNDLE_EXE=$tauriExe",
    "-DBUNDLE_OUT=$bundleDir",
    '-DBUNDLE_CONF=Release',
    '-DBUNDLE_WIN=1',
    '-DBUNDLE_UI_KIND=tauri2',
    "-DBUNDLE_SCRIPTS=$(Join-Path $repoRoot 'core/python/scripts')",
    "-DBUNDLE_CATALOG=$catalogExe",
    "-DBUNDLE_RUNTIME=$($releasePaths.runtime_dir)",
    "-DBUNDLE_BACKENDS=$backendExe",
    "-DBUNDLE_TOOLS=$catalogExe",
    '-P', (Join-Path $repoRoot 'tools/bundle.cmake')
)

# 在压 zip 前用随包 CLI 解析随包脚本；声明错误不能等到用户启动后才发现。
$bundledCatalog = Join-Path $bundleDir 'tools/go_script_catalog/go_script_catalog.exe'
Invoke-Checked $bundledCatalog @('--check', '--root', (Join-Path $bundleDir 'scripts'))

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Push-Location $bundleDir
try { Invoke-Checked 'cmake' @('-E', 'tar', 'cf', $zip, '--format=zip', '.') }
finally { Pop-Location }
Invoke-Checked 'pwsh' @(
    '-NoProfile',
    '-File', (Join-Path $repoRoot 'tools/verify_slim_zip.ps1'),
    '-ZipPath', $zip, '-RepoRoot', $repoRoot, '-UiKind', 'tauri2'
)
Write-Host "[GO] Tauri 2 发行包：$zip"
