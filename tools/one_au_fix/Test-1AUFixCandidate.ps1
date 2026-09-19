[CmdletBinding()]
param([Parameter(Mandatory)][string]$CandidateRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$builder = Join-Path $PSScriptRoot 'New-1AUFixCandidate.ps1'
$patch = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'candidate-patch.json') -Raw | ConvertFrom-Json
$testRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ('..\..\build\unit\one-au-fix-' + [Guid]::NewGuid().ToString('N'))))
$output = Join-Path $testRoot 'Dawn-0.1.3-1au-fix'
$originalDll = Join-Path $CandidateRoot 'payload\steam_api64.dll'
$originalHash = (Get-FileHash -LiteralPath $originalDll -Algorithm SHA256).Hash
$built = & $builder -CandidateRoot $CandidateRoot -OutputDirectory $output
$manifest = Get-Content -LiteralPath (Join-Path $output 'release.json') -Raw | ConvertFrom-Json
if ($manifest.release -ne '0.1.3-1au-fix') { throw 'Wrong output release.' }
foreach ($file in $manifest.files) {
    $path = Join-Path (Join-Path $output 'payload') $file.path
    if ((Get-Item -LiteralPath $path).Length -ne $file.size -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) { throw 'Output manifest mismatch.' }
    if ($file.path -ne 'steam_api64.dll' -and (Get-FileHash -LiteralPath (Join-Path (Join-Path $CandidateRoot 'payload') $file.path)).Hash -ne $file.sha256) { throw 'Unrelated payload changed.' }
}
if ((Get-FileHash -LiteralPath (Join-Path $output 'payload\steam_api64.dll')).Hash -ne $patch.patchedSha256) { throw 'Final DLL differs from verified gameplay version.' }
if ((Get-FileHash -LiteralPath $originalDll).Hash -ne $originalHash) { throw 'Original candidate changed.' }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($built.zip)
try {
    foreach ($file in $manifest.files) {
        $entryName = 'Dawn-0.1.3-1au-fix/payload/' + $file.path.Replace('\', '/')
        $entry = $zip.GetEntry($entryName)
        if ($null -eq $entry -or $entry.Length -ne $file.size) { throw "Missing ZIP payload: $entryName" }
    }
    $entry = $zip.GetEntry('Dawn-0.1.3-1au-fix/payload/steam_api64.dll')
    $stream = $entry.Open()
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $zipHash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') }
    finally { $sha.Dispose(); $stream.Dispose() }
    if ($zipHash -ne $patch.patchedSha256) { throw 'ZIP DLL checksum mismatch.' }
} finally { $zip.Dispose() }

function Expect-Failure([string]$Expected, [scriptblock]$Action) {
    $failed = $false
    try { & $Action | Out-Null }
    catch {
        if ($_.Exception.Message -notlike $Expected) { throw }
        $failed = $true
    }
    if (-not $failed) { throw "Expected rejection: $Expected" }
}
Expect-Failure 'Output already exists*' { & $builder -CandidateRoot $CandidateRoot -OutputDirectory $output }
Expect-Failure 'Output must be separate*' { & $builder -CandidateRoot $CandidateRoot -OutputDirectory (Join-Path $CandidateRoot 'nested-output') }

# A self-consistent package containing a different DLL must still be refused.
$different = Join-Path $testRoot 'different-build'
Copy-Item -LiteralPath $output -Destination $different -Recurse
$differentManifest = Join-Path $different 'release.json'
$changed = Get-Content -LiteralPath $differentManifest -Raw | ConvertFrom-Json
$changed.release = $patch.candidate
[IO.File]::WriteAllText($differentManifest, ($changed | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding($false)))
$rejectedOutput = Join-Path $testRoot 'must-not-exist'
Expect-Failure 'Unsupported DLL*' { & $builder -CandidateRoot $different -OutputDirectory $rejectedOutput }
if (Test-Path -LiteralPath $rejectedOutput) { throw 'Rejected input created an output directory.' }
if ((Get-FileHash -LiteralPath (Join-Path $output 'payload\steam_api64.dll')).Hash -ne $patch.patchedSha256) { throw 'Rejection tests altered the good output.' }
[pscustomobject]@{
    payloadFilesVerified = @($manifest.files).Count
    dllMatchesVerifiedGameplay = $true
    zipDllVerified = $true
    originalInputPreserved = $true
    otherPayloadPreserved = $true
    refusesExistingOutput = $true
    refusesNestedOutput = $true
    refusesDifferentBuildBeforeWriting = $true
} | ConvertTo-Json
