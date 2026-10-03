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
$QtBin = Join-Path $env:QT_ROOT_DIR 'bin'
if (-not (Test-Path $QtBin -PathType Container)) {
    throw "Qt binary directory was not found: $QtBin"
}
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

$stageFiles = [System.Collections.Generic.Dictionary[string, string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase
)
$pending = [System.Collections.Generic.Queue[string]]::new()
$visited = @{}

function Add-PeFilesToQueue {
    foreach ($file in (Get-ChildItem -Path $stage -Recurse -File)) {
        if ($file.Extension -notin @('.dll', '.exe')) { continue }
        if (-not $stageFiles.ContainsKey($file.Name) -or
            [IO.Path]::GetDirectoryName($file.FullName) -eq $stage) {
            [void]($stageFiles[$file.Name] = $file.FullName)
        }
        $pending.Enqueue($file.FullName)
    }
}

function Get-Dependencies([string]$Path) {
    $outputLines = & dumpbin.exe /nologo /dependents $Path
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed for $Path" }
    foreach ($line in $outputLines) {
        if ($line -match '^\s+([A-Za-z0-9_.+-]+\.dll)\s*$') { $Matches[1] }
    }
}

function Add-DependencyToStage([string]$Name, [string]$FromPath) {
    if ($stageFiles.ContainsKey($Name)) {
        $existing = $stageFiles[$Name]
        if ([IO.Path]::GetDirectoryName($existing) -ne $stage) {
            $destination = Join-Path $stage $Name
            if (-not (Test-Path $destination)) {
                Copy-Item $existing $destination
                [void]($stageFiles[$Name] = $destination)
                $pending.Enqueue($destination)
            } else {
                [void]($stageFiles[$Name] = $destination)
            }
        }
        return $true
    }

    foreach ($sourceDirectory in @($VcpkgBin, $QtBin)) {
        $source = Join-Path $sourceDirectory $Name
        if (Test-Path $source -PathType Leaf) {
            $destination = Join-Path $stage $Name
            Copy-Item $source $destination
            [void]($stageFiles[$Name] = $destination)
            $pending.Enqueue($destination)
            return $true
        }
    }

    $lowerName = $Name.ToLowerInvariant()
    if ($lowerName.StartsWith('api-ms-win-') -or
        $lowerName.StartsWith('ext-ms-win-')) {
        return $true
    }

    $requiresAppLocalRuntime = $lowerName -match '^(vcruntime|msvcp|concrt).+\.dll$'
    $systemDll = Join-Path $env:SystemRoot "System32\$Name"
    if (-not $requiresAppLocalRuntime -and (Test-Path $systemDll -PathType Leaf)) {
        return $true
    }

    throw "unresolved non-system runtime dependency '$Name' required by '$FromPath'"
}

Add-PeFilesToQueue
while ($pending.Count -gt 0) {
    $Path = $pending.Dequeue()
    $key = [IO.Path]::GetFullPath($Path).ToLowerInvariant()
    if ($visited.ContainsKey($key)) { continue }
    [void]($visited[$key] = $true)

    $headers = & dumpbin.exe /nologo /headers $Path
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed for $Path" }
    if (-not ($headers | Select-String -Quiet '8664 machine \(x64\)')) {
        throw "packaged PE file is not x64: $Path"
    }

    foreach ($name in (Get-Dependencies $Path)) {
        if (-not (Add-DependencyToStage $name $Path)) {
            throw "failed to stage runtime dependency '$name' required by '$Path'"
        }
    }
}

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
$env:PATH = "$verificationDir;$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
$env:QT_PLUGIN_PATH = ''
$env:QT_QPA_PLATFORM_PLUGIN_PATH = ''
$env:QML_IMPORT_PATH = ''
$env:QML2_IMPORT_PATH = ''
$env:QT_ROOT_DIR = ''
$env:QTDIR = ''
$env:QT_QPA_PLATFORM = 'offscreen'
& $packagedExe --ci-smoke-test
if ($LASTEXITCODE -ne 0) { throw "packaged BrockDJ smoke test failed with exit code $LASTEXITCODE" }
