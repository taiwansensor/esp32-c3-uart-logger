[CmdletBinding()]
param(
    [string]$HostName = "uart-logger.local",
    [ValidateRange(1, 65535)]
    [int]$Port = 2323,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\captures"),
    [ValidateRange(1, 60)]
    [int]$ReconnectDelaySeconds = 2
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($resolvedOutput) | Out-Null
$safeHostName = $HostName -replace '[^A-Za-z0-9._-]', '_'

Write-Host "UART TCP auto-capture"
Write-Host "Target: ${HostName}:$Port"
Write-Host "Output: $resolvedOutput"
Write-Host "Press Ctrl+C to stop."

while ($true) {
    $client = $null
    $stream = $null
    $outputFile = $null
    try {
        $client = [System.Net.Sockets.TcpClient]::new()
        $connectTask = $client.ConnectAsync($HostName, $Port)
        if (-not $connectTask.Wait(5000)) {
            throw "Connection timed out after 5 seconds."
        }
        if ($connectTask.IsFaulted) {
            throw $connectTask.Exception.GetBaseException()
        }

        $client.NoDelay = $true
        $stream = $client.GetStream()
        $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $path = Join-Path $resolvedOutput "uart-${safeHostName}-${timestamp}.log"
        $outputFile = [System.IO.File]::Open(
            $path,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::Read
        )
        Write-Host "`n[$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')] Connected; writing $path" -ForegroundColor Green

        $buffer = [byte[]]::new(4096)
        while ($client.Connected) {
            $readTask = $stream.ReadAsync($buffer, 0, $buffer.Length)
            while (-not $readTask.Wait(500)) {
                # Short waits keep Ctrl+C responsive while no UART data arrives.
            }
            $count = $readTask.Result
            if ($count -le 0) {
                break
            }

            $outputFile.Write($buffer, 0, $count)
            $outputFile.Flush()
            [Console]::Write([System.Text.Encoding]::UTF8.GetString($buffer, 0, $count))
        }
        Write-Warning "TCP connection closed."
    }
    catch {
        Write-Warning "[$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')] $($_.Exception.Message)"
    }
    finally {
        if ($null -ne $outputFile) {
            $outputFile.Flush()
            $outputFile.Dispose()
        }
        if ($null -ne $stream) {
            $stream.Dispose()
        }
        if ($null -ne $client) {
            $client.Dispose()
        }
    }

    Write-Host "Reconnect in $ReconnectDelaySeconds seconds..."
    Start-Sleep -Seconds $ReconnectDelaySeconds
}
