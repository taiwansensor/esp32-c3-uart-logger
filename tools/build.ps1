[CmdletBinding()]
param(
    [string]$ArduinoCli = "",
    [string]$ArduinoConfig = "",
    [string]$BuildPath = "",
    [string]$Port = "",
    [switch]$Upload
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if ([string]::IsNullOrWhiteSpace($BuildPath)) {
    $BuildPath = Join-Path $projectDirectory ".arduino-build"
}
$fqbn = "esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio,FlashSize=4M,PartitionScheme=default"

if ([string]::IsNullOrWhiteSpace($ArduinoCli)) {
    $arduinoCommand = Get-Command "arduino-cli" -ErrorAction SilentlyContinue
    if ($null -eq $arduinoCommand) {
        throw "arduino-cli was not found in PATH. Use -ArduinoCli to provide its path."
    }
    $ArduinoCli = $arduinoCommand.Source
}
elseif (-not (Test-Path -LiteralPath $ArduinoCli -PathType Leaf)) {
    throw "arduino-cli not found: $ArduinoCli"
}

$compileArguments = @("compile", "--fqbn", $fqbn, "--build-path", $BuildPath)
if (-not [string]::IsNullOrWhiteSpace($ArduinoConfig)) {
    if (-not (Test-Path -LiteralPath $ArduinoConfig -PathType Leaf)) {
        throw "Arduino CLI config not found: $ArduinoConfig"
    }
    $compileArguments += @("--config-file", $ArduinoConfig)
}
$compileArguments += $projectDirectory
& $ArduinoCli @compileArguments
if ($LASTEXITCODE -ne 0) {
    throw "Compile failed with exit code $LASTEXITCODE"
}

if ($Upload) {
    if ([string]::IsNullOrWhiteSpace($Port)) {
        throw "-Upload requires -Port, for example COM9."
    }
    $uploadArguments = @("upload", "--fqbn", $fqbn, "--port", $Port, "--input-dir", $BuildPath)
    if (-not [string]::IsNullOrWhiteSpace($ArduinoConfig)) {
        $uploadArguments += @("--config-file", $ArduinoConfig)
    }
    $uploadArguments += $projectDirectory
    & $ArduinoCli @uploadArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Upload failed with exit code $LASTEXITCODE"
    }
}
