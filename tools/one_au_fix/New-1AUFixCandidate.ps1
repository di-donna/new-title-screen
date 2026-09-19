[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CandidateRoot,
    [string]$OutputDirectory = '',
    [ValidatePattern('^$|^[0-9a-fA-F]{40}$')][string]$SourceCommit = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot '..\..\build\releases\Dawn-0.1.3-1au-fix'
}
$sourceRoot = [IO.Path]::GetFullPath($CandidateRoot).TrimEnd('\', '/')
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\', '/')
$zipPath = $outputRoot + '.zip'
if ($outputRoot -eq $sourceRoot -or $outputRoot.StartsWith($sourceRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Output must be separate from the input candidate.'
}
if ((Test-Path -LiteralPath $outputRoot) -or (Test-Path -LiteralPath $zipPath)) {
    throw 'Output already exists. Choose a new output directory; nothing will be overwritten.'
}

function Child-Path([string]$Root, [string]$Relative) {
    $path = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if (-not $path.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package path escapes its directory: $Relative"
    }
    return $path
}
function Sha256([byte[]]$Bytes) {
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $hash.Dispose() }
}
function Hex-Bytes([string]$Hex) {
    if ($Hex.Length % 2 -ne 0 -or $Hex -notmatch '^[0-9a-f]+$') { throw 'Invalid patch hex.' }
    $bytes = New-Object byte[] ($Hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [Convert]::ToByte($Hex.Substring(2 * $i, 2), 16) }
    return ,$bytes
}
function Write-Utf8([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}

$patch = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'candidate-patch.json') -Raw | ConvertFrom-Json
$release = Get-Content -LiteralPath (Join-Path $sourceRoot 'release.json') -Raw | ConvertFrom-Json
if ($patch.schema -ne 1 -or $release.schema -ne 1 -or $release.gameBuild -ne 86657 -or $release.runtimeDirectory -ne 'Dawn') {
    throw 'Unsupported patch or release manifest.'
}
if ($release.release -ne $patch.candidate) { throw 'Requires the original 0.1.3-1au-candidate package; rebuilt DLLs require the source port.' }
$payloadRoot = Child-Path $sourceRoot 'payload'
$dllPath = Child-Path $payloadRoot 'steam_api64.dll'
$dll = [IO.File]::ReadAllBytes($dllPath)
if ($dll.Length -ne $patch.size -or (Sha256 $dll) -ne $patch.originalSha256) {
    throw 'Unsupported DLL. Only the exact original 0.1.3 1AU candidate can be patched. Nothing was written.'
}
$files = @($release.files)
$dllEntry = @($files | Where-Object { $_.path -eq 'steam_api64.dll' })
if ($dllEntry.Count -ne 1) { throw 'Expected exactly one DLL entry.' }
foreach ($file in $files) {
    $path = Child-Path $payloadRoot $file.path
    $item = Get-Item -LiteralPath $path
    if ($item.PSIsContainer -or $item.Length -ne $file.size -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "Input payload failed validation: $($file.path)"
    }
}
$launchers = @('Install-Dawn.cmd', 'Install-Dawn.ps1', 'Update-Dawn.cmd', 'Update-Dawn.ps1', 'READ-ME.txt')
foreach ($name in $launchers) {
    if (-not (Test-Path -LiteralPath (Child-Path $sourceRoot $name) -PathType Leaf)) { throw "Missing candidate installer: $name" }
}

$previousEnd = 0L
foreach ($range in $patch.ranges) {
    $before = Hex-Bytes $range.before
    $after = Hex-Bytes $range.after
    $offset = [long]$range.offset
    if ($offset -lt $previousEnd -or $before.Length -ne $after.Length -or $offset -gt $dll.Length - $before.Length) {
        throw 'Invalid or overlapping patch range.'
    }
    for ($i = 0; $i -lt $before.Length; $i++) {
        if ($dll[$offset + $i] -ne $before[$i]) { throw "Unexpected input bytes at $offset" }
    }
    [Array]::Copy($after, 0, $dll, $offset, $after.Length)
    $previousEnd = $offset + $after.Length
}
if ((Sha256 $dll) -ne $patch.patchedSha256) { throw 'Final DLL checksum mismatch. Nothing was written.' }

# All source validation and patch checks finish before any output is created.
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$outputPayload = Child-Path $outputRoot 'payload'
foreach ($file in $files) {
    $source = Child-Path $payloadRoot $file.path
    $target = Child-Path $outputPayload $file.path
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
    if ($file.path -eq 'steam_api64.dll') { [IO.File]::WriteAllBytes($target, $dll) }
    else { [IO.File]::Copy($source, $target, $false) }
}
foreach ($name in $launchers) { [IO.File]::Copy((Child-Path $sourceRoot $name), (Child-Path $outputRoot $name), $false) }
$release.release = '0.1.3-1au-fix'
$release.createdUtc = [DateTime]::UtcNow.ToString('o')
$dllEntry[0].size = $dll.Length
$dllEntry[0].sha256 = $patch.patchedSha256
Write-Utf8 (Join-Path $outputRoot 'release.json') ($release | ConvertTo-Json -Depth 10)
[IO.File]::Copy((Join-Path $PSScriptRoot 'TESTING.md'), (Join-Path $outputRoot 'RELEASE-NOTES.md'), $false)

$recipeOutput = Child-Path $outputRoot 'repair-source'
[IO.Directory]::CreateDirectory($recipeOutput) | Out-Null
foreach ($name in @('candidate-patch.json', 'verification.json', 'New-1AUFixCandidate.ps1', 'Test-1AUFixCandidate.ps1', 'TESTING.md', 'SOURCE-PORT.md', 'README.md')) {
    [IO.File]::Copy((Join-Path $PSScriptRoot $name), (Join-Path $recipeOutput $name), $false)
}
foreach ($name in @('reference', 'source-port')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $recipeOutput -Recurse
}
$licensePath = Join-Path $PSScriptRoot '..\..\LICENSE'
if (-not (Test-Path -LiteralPath $licensePath -PathType Leaf)) { $licensePath = Join-Path $PSScriptRoot 'LICENSE' }
[IO.File]::Copy($licensePath, (Join-Path $recipeOutput 'LICENSE'), $false)
Write-Utf8 (Join-Path $recipeOutput 'build.json') ([pscustomobject]@{
    branch='1AU-fix'; sourceCommit=$SourceCommit; sourcePortComplete=$false;
    recipeSha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'candidate-patch.json')).Hash.ToLowerInvariant();
    originalDllSha256=$patch.originalSha256; patchedDllSha256=$patch.patchedSha256
} | ConvertTo-Json)
foreach ($file in $release.files) {
    $path = Child-Path $outputPayload $file.path
    if ((Get-Item -LiteralPath $path).Length -ne $file.size -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "Output payload failed verification: $($file.path)"
    }
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $outputRoot -File -Recurse) {
        $relative = $file.FullName.Substring($outputRoot.Length).TrimStart('\', '/').Replace('\', '/')
        $entryName = [IO.Path]::GetFileName($outputRoot) + '/' + $relative
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entryName, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $archive.Dispose() }
[pscustomobject]@{ release=$release.release; directory=$outputRoot; zip=$zipPath; dllSha256=$patch.patchedSha256; sourcePortComplete=$false }
