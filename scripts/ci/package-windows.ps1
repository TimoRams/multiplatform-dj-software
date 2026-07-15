param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$VcpkgBin,
    [Parameter(Mandatory = $true)][string]$QmlDir,
    [Parameter(Mandatory = $true)][string]$Output
)

$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path $Executable).Path
$VcpkgBin = (Resolve-Path $VcpkgBin).Path
$QmlDir = (Resolve-Path $QmlDir).Path
$Output = [IO.Path]::GetFullPath($Output)
$stage = Join-Path ([IO.Path]::GetDirectoryName($Output)) 'BrockDJ-Windows-x64'

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
$stagedExe = Join-Path $stage 'BrockDJ.exe'
Copy-Item $Executable $stagedExe

$windeployqt = Join-Path $env:QT_ROOT_DIR 'bin\windeployqt.exe'
if (-not (Test-Path $windeployqt)) {
    $windeployqt = (Get-Command windeployqt.exe -ErrorAction Stop).Source
}
& $windeployqt --release --qmldir $QmlDir --compiler-runtime --no-translations $stagedExe
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }

$systemDlls = @(
    'ADVAPI32.dll', 'bcrypt.dll', 'COMCTL32.dll', 'COMDLG32.dll', 'CRYPT32.dll',
    'DWMAPI.dll', 'GDI32.dll', 'IMM32.dll', 'IPHLPAPI.DLL', 'KERNEL32.dll',
    'NETAPI32.dll', 'ntdll.dll', 'OLE32.dll', 'OLEAUT32.dll', 'POWRPROF.dll',
    'PSAPI.DLL', 'Secur32.dll', 'SETUPAPI.dll', 'SHELL32.dll', 'SHLWAPI.dll',
    'USER32.dll', 'USERENV.dll', 'UxTheme.dll', 'VERSION.dll', 'WININET.dll',
    'WINMM.dll', 'WS2_32.dll', 'WTSAPI32.dll'
)
$systemDlls = $systemDlls | ForEach-Object { $_.ToLowerInvariant() }
$visited = @{}

function Get-Dependencies([string]$Path) {
    $outputLines = & dumpbin.exe /nologo /dependents $Path
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed for $Path" }
    foreach ($line in $outputLines) {
        if ($line -match '^\s+([A-Za-z0-9_.+-]+\.dll)\s*$') { $Matches[1] }
    }
}

function Bundle-Dependencies([string]$Path) {
    $key = [IO.Path]::GetFullPath($Path).ToLowerInvariant()
    if ($visited.ContainsKey($key)) { return }
    $visited[$key] = $true

    foreach ($name in (Get-Dependencies $Path)) {
        $destination = Join-Path $stage $name
        if (Test-Path $destination) {
            Bundle-Dependencies $destination
            continue
        }

        $source = Join-Path $VcpkgBin $name
        if (Test-Path $source) {
            Copy-Item $source $destination
            Bundle-Dependencies $destination
            continue
        }

        $lowerName = $name.ToLowerInvariant()
        if ($lowerName.StartsWith('api-ms-win-') -or
            $lowerName.StartsWith('ext-ms-win-') -or
            $systemDlls -contains $lowerName) {
            continue
        }
        throw "unresolved non-system runtime dependency '$name' required by '$Path'"
    }
}

Bundle-Dependencies $stagedExe

if (Test-Path $Output) { Remove-Item -Force $Output }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $Output -CompressionLevel Optimal
if (-not (Test-Path $Output) -or (Get-Item $Output).Length -eq 0) {
    throw "Windows ZIP was not created: $Output"
}

# Verify and launch the executable from the exact archive users will download.
$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$verificationDir = Join-Path $tempRoot 'brockdj-windows-package-smoke'
if (Test-Path $verificationDir) { Remove-Item -Recurse -Force $verificationDir }
Expand-Archive -Path $Output -DestinationPath $verificationDir
$packagedExe = Join-Path $verificationDir 'BrockDJ.exe'
if (-not (Test-Path $packagedExe)) { throw 'BrockDJ.exe is missing from the Windows ZIP' }
$env:QT_QPA_PLATFORM = 'offscreen'
& $packagedExe --ci-smoke-test
if ($LASTEXITCODE -ne 0) { throw "packaged BrockDJ smoke test failed with exit code $LASTEXITCODE" }
