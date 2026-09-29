param(
    [Parameter(Mandatory = $true)][string]$ZipPath,
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [ValidateSet('qt5', 'tauri2')][string]$UiKind = 'tauri2'
)

$ErrorActionPreference = 'Stop'

function Normalize-ZipName([string]$Name) {
    $normalized = $Name.Replace('\', '/')
    while ($normalized.StartsWith('./', [System.StringComparison]::Ordinal)) {
        $normalized = $normalized.Substring(2)
    }
    return $normalized.TrimEnd('/')
}

function Read-ZipCentralDirectoryCrc([string]$Path) {
    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read
    )
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        $tailLength = [int][Math]::Min([long]$stream.Length, [long]65557)
        [void]$stream.Seek(-$tailLength, [System.IO.SeekOrigin]::End)
        $tail = $reader.ReadBytes($tailLength)

        $eocdOffset = -1
        for ($i = $tail.Length - 22; $i -ge 0; $i--) {
            if ([System.BitConverter]::ToUInt32($tail, $i) -ne [uint32]0x06054b50) { continue }
            $commentLength = [System.BitConverter]::ToUInt16($tail, $i + 20)
            if ($i + 22 + $commentLength -eq $tail.Length) {
                $eocdOffset = $i
                break
            }
        }
        if ($eocdOffset -lt 0) { throw '找不到 ZIP End of Central Directory 记录' }

        $diskNumber = [System.BitConverter]::ToUInt16($tail, $eocdOffset + 4)
        $centralDisk = [System.BitConverter]::ToUInt16($tail, $eocdOffset + 6)
        $diskEntryCount = [System.BitConverter]::ToUInt16($tail, $eocdOffset + 8)
        $entryCount = [System.BitConverter]::ToUInt16($tail, $eocdOffset + 10)
        $centralSize = [System.BitConverter]::ToUInt32($tail, $eocdOffset + 12)
        $centralOffset = [System.BitConverter]::ToUInt32($tail, $eocdOffset + 16)
        if ($diskNumber -ne 0 -or $centralDisk -ne 0 -or $diskEntryCount -ne $entryCount) {
            throw '不支持分卷 ZIP'
        }
        if ($entryCount -eq 0xffff -or $centralSize -eq [uint32]::MaxValue -or
            $centralOffset -eq [uint32]::MaxValue) {
            throw '发行包超出校验器支持的 ZIP64 范围'
        }
        if ([long]$centralOffset + [long]$centralSize -gt $stream.Length) {
            throw 'ZIP 中央目录越过文件末尾'
        }

        [void]$stream.Seek([long]$centralOffset, [System.IO.SeekOrigin]::Begin)
        $crcs = @{}
        for ($entryIndex = 0; $entryIndex -lt $entryCount; $entryIndex++) {
            $header = $reader.ReadBytes(46)
            if ($header.Length -ne 46 -or
                [System.BitConverter]::ToUInt32($header, 0) -ne [uint32]0x02014b50) {
                throw 'ZIP 中央目录记录损坏'
            }
            $flags = [System.BitConverter]::ToUInt16($header, 8)
            $crc = [System.BitConverter]::ToUInt32($header, 16)
            $nameLength = [System.BitConverter]::ToUInt16($header, 28)
            $extraLength = [System.BitConverter]::ToUInt16($header, 30)
            $commentLength = [System.BitConverter]::ToUInt16($header, 32)
            $nameBytes = $reader.ReadBytes($nameLength)
            if ($nameBytes.Length -ne $nameLength) { throw 'ZIP 中央目录文件名被截断' }
            if (($flags -band 0x0800) -ne 0) {
                $rawName = [System.Text.Encoding]::UTF8.GetString($nameBytes)
            }
            else {
                $rawName = [System.Text.Encoding]::ASCII.GetString($nameBytes)
            }
            $key = Normalize-ZipName $rawName
            if ($crcs.ContainsKey($key)) { throw "ZIP 中有重复条目：$key" }
            $crcs[$key] = $crc
            [void]$reader.ReadBytes($extraLength + $commentLength)
        }
        return $crcs
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

if (-not ('GoSlimZipCrc32' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.IO;

public static class GoSlimZipCrc32
{
    private static readonly uint[] Table = BuildTable();

    private static uint[] BuildTable()
    {
        var table = new uint[256];
        for (int i = 0; i < table.Length; i++)
        {
            uint value = (uint)i;
            for (int bit = 0; bit < 8; bit++)
                value = (value & 1) != 0 ? 0xedb88320U ^ (value >> 1) : value >> 1;
            table[i] = value;
        }
        return table;
    }

    public static uint Compute(Stream stream, out long length)
    {
        uint crc = 0xffffffffU;
        length = 0;
        byte[] buffer = new byte[65536];
        int count;
        while ((count = stream.Read(buffer, 0, buffer.Length)) > 0)
        {
            length += count;
            for (int i = 0; i < count; i++)
                crc = Table[(int)((crc ^ buffer[i]) & 0xff)] ^ (crc >> 8);
        }
        return ~crc;
    }
}
'@
}

try {
    $resolvedZip = [System.IO.Path]::GetFullPath($ZipPath)
    $resolvedRepo = [System.IO.Path]::GetFullPath($RepoRoot)
    if (-not (Test-Path -LiteralPath $resolvedZip -PathType Leaf)) {
        throw "发行 zip 不存在：$resolvedZip"
    }

    $scriptsRoot = Join-Path $resolvedRepo 'core/python/scripts'
    $sourceScripts = @(Get-ChildItem -LiteralPath $scriptsRoot -Recurse -File -Filter '*.py')
    $frontendExe = if ($UiKind -eq 'qt5') {
        'GeneralOperationsLauncher.exe'
    }
    else {
        'general-operations-tauri.exe'
    }
    $required = @{
        'requirements.txt' = $true
        'python-launcher.ps1' = $true
        'runtime-check.py' = $true
        'tools/go_pmx2glb/go_pmx2glb.exe' = $true
    }
    $required[$frontendExe] = $true
    if ($UiKind -eq 'tauri2') {
        $required['tools/go_script_catalog/go_script_catalog.exe'] = $true
    }
    foreach ($script in $sourceScripts) {
        $relative = $script.FullName.Substring($scriptsRoot.Length).TrimStart('\', '/')
        $required[(Normalize-ZipName ("scripts/$relative"))] = $true
    }

    $centralCrcs = Read-ZipCentralDirectoryCrc $resolvedZip
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($resolvedZip)
    try {
        $names = @{}
        $fileNames = @{}
        foreach ($entry in $archive.Entries) {
            $name = Normalize-ZipName $entry.FullName
            if ($names.ContainsKey($name)) { throw "ZIP 中有重复条目：$name" }
            $names[$name] = $true
            if (-not ($entry.FullName.EndsWith('/') -or $entry.FullName.EndsWith('\'))) {
                $fileNames[$name] = $true
            }

            if (-not $centralCrcs.ContainsKey($name)) { throw "ZIP 条目缺少中央目录 CRC：$name" }
            $entryStream = $entry.Open()
            try {
                [long]$actualLength = 0
                $actualCrc = [GoSlimZipCrc32]::Compute($entryStream, [ref]$actualLength)
            }
            finally { $entryStream.Dispose() }
            if ($actualLength -ne $entry.Length) { throw "ZIP 条目解压长度不符：$name" }
            if ([uint32]$actualCrc -ne [uint32]$centralCrcs[$name]) { throw "ZIP 条目 CRC 错误：$name" }
        }
        if ($archive.Entries.Count -ne $centralCrcs.Count) {
            throw 'ZIP 中央目录条目数与归档读取结果不一致'
        }

        $missing = @($required.Keys | Where-Object { -not $fileNames.ContainsKey($_) } | Sort-Object)
        if ($missing.Count -gt 0) { throw "发行 zip 缺少必要文件：$($missing -join ', ')" }

        $forbidden = @($names.Keys | Where-Object {
            $_ -eq 'python' -or $_.StartsWith('python/', [System.StringComparison]::OrdinalIgnoreCase) -or
            $_ -eq 'tools/realesrgan' -or $_.StartsWith('tools/realesrgan/', [System.StringComparison]::OrdinalIgnoreCase)
        } | Sort-Object)
        if ($forbidden.Count -gt 0) { throw "发行 zip 含有已移除目录：$($forbidden -join ', ')" }

        if ($UiKind -eq 'tauri2') {
            $unexpectedQt = @($names.Keys | Where-Object {
                $_ -eq 'qt.conf' -or $_.StartsWith('Qt5', [System.StringComparison]::OrdinalIgnoreCase)
            } | Sort-Object)
            if ($unexpectedQt.Count -gt 0) { throw "Tauri zip 混入 Qt5 运行时：$($unexpectedQt -join ', ')" }
        }
    }
    finally { $archive.Dispose() }

    Write-Host "[GO] slim zip 校验通过：$($sourceScripts.Count) 个脚本，归档 CRC 正常"
    exit 0
}
catch {
    [Console]::Error.WriteLine("[GO] slim zip 校验失败：$($_.Exception.Message)")
    exit 1
}
