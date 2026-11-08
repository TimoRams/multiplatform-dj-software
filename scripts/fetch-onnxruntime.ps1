$ErrorActionPreference = 'Stop'
$version = '1.31.1'
$asset = "onnxruntime-win-x64-$version.zip"
$expectedHash = '5869e26bb25314375f2d5ef1877e3ea06b1543d6c710e6e4a1ec2f7177ab4b2c'
$root = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $root 'libs/onnxruntime'

if (Test-Path -LiteralPath $destination) {
    throw "Refusing to overwrite the existing ONNX Runtime SDK at $destination. Move it elsewhere first."
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) "brockdj-onnxruntime-$([guid]::NewGuid())"
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    $archive = Join-Path $temporary $asset
    $url = "https://github.com/microsoft/onnxruntime/releases/download/v$version/$asset"
    Invoke-WebRequest -Uri $url -OutFile $archive
    $actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    if ($actualHash -ne $expectedHash) {
        throw "ONNX Runtime archive checksum mismatch: $actualHash"
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $temporary
    Move-Item -LiteralPath (Join-Path $temporary "onnxruntime-win-x64-$version") -Destination $destination
    Write-Host "ONNX Runtime $version installed to $destination. Re-run CMake to enable STEMS."
}
finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
