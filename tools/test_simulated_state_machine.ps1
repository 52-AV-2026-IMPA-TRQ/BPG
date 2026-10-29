[CmdletBinding()]
param(
    [string]$BaseUrl = 'http://dashboard.local',
    [int]$TimeoutSeconds = 15
)

# Simulated state machine check: only uses the REST API, no hardware assumptions.
$ErrorActionPreference = 'Stop'
$BaseUrl = $BaseUrl.TrimEnd('/')

function Fail([string]$Message) {
    throw "FAIL: $Message"
}

function Get-BenchStatus {
    Invoke-RestMethod -Uri "$BaseUrl/api/v1/bench/status" -Method Get -TimeoutSec 5
}

function Send-BenchCommand([string]$Command) {
    $body = @{ command = $Command } | ConvertTo-Json -Compress
    try {
        $response = Invoke-RestMethod -Uri "$BaseUrl/api/v1/bench/command" -Method Post `
            -ContentType 'application/json' -Body $body -TimeoutSec 5
    } catch {
        Fail "command $Command was not accepted: $($_.Exception.Message)"
    }
    if (-not $response.accepted -or $response.command -ne $Command) {
        Fail "unexpected response to command ${Command}: $($response | ConvertTo-Json -Compress)"
    }
    Write-Host "Sent $Command"
}

# Polls until the predicate matches; returns the matching status and the states seen.
function Wait-BenchStatus([scriptblock]$Predicate, [string]$Description) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $seen = New-Object System.Collections.Generic.List[string]
    do {
        $status = Get-BenchStatus
        if ($seen.Count -eq 0 -or $seen[$seen.Count - 1] -ne $status.state) {
            $seen.Add($status.state)
        }
        if (& $Predicate $status) {
            return [pscustomobject]@{ Status = $status; Seen = $seen }
        }
        Start-Sleep -Milliseconds 50
    } while ((Get-Date) -lt $deadline)

    $last = Get-BenchStatus
    Fail "timeout waiting for $Description (last state: $($last.state), seen: $($seen -join ' -> '))"
}

function Get-HttpStatusCode($ErrorRecord) {
    $response = $null
    if ($null -ne $ErrorRecord -and $null -ne $ErrorRecord.Exception) {
        $response = $ErrorRecord.Exception.Response
    }
    if ($null -eq $response) { return $null }

    $status = $null
    try { $status = $response.StatusCode } catch { return $null }
    if ($null -eq $status) { return $null }

    try { return [int]$status.value__ } catch { }
    try { return [int]$status } catch { return $null }
}

function Get-ErrorDiagnostics($ErrorRecord) {
    $exception = $ErrorRecord.Exception
    $responseType = 'none'
    if ($null -ne $exception.Response) { $responseType = $exception.Response.GetType().FullName }
    return "exception type: $($exception.GetType().FullName); message: $($exception.Message); response type: $responseType"
}

# Succeeds only when the POST fails with exactly the expected HTTP status.
function Assert-PostStatus([string]$Path, [string]$Body, [int]$Expected, [string]$Description) {
    $code = $null
    $diagnostics = ''
    try {
        $null = Invoke-RestMethod -Uri "$BaseUrl$Path" -Method Post -ContentType 'application/json' `
            -Body $Body -TimeoutSec 5
        $code = 200
    } catch {
        $code = Get-HttpStatusCode $_
        $diagnostics = Get-ErrorDiagnostics $_
    }
    if ($null -eq $code) {
        Fail "${Description}: could not determine HTTP status code, expected $Expected ($diagnostics)"
    }
    if ($code -ne $Expected) {
        Fail "$Description returned HTTP $code, expected $Expected"
    }
    $s = Get-BenchStatus
    if ($s.state -ne 'READY' -or $s.contactorActive -or $s.pwmPercent -ne 0) {
        Fail "bench not safe in READY after '$Description' (state $($s.state), contactorActive $($s.contactorActive), pwmPercent $($s.pwmPercent))"
    }
    Write-Host "OK: $Description -> HTTP $Expected"
}

function Test-NegativeRequests {
    Assert-PostStatus '/api/v1/bench/command' '{"command":"INVALID"}' 400 'command INVALID'
    Assert-PostStatus '/api/v1/bench/command' '{"command":"STOP","extra":1}' 400 'command with extra field'
    Assert-PostStatus '/api/v1/bench/simulation' '{"currentA":-1}' 400 'simulation with negative currentA'
    Assert-PostStatus '/api/v1/bench/simulation' '{"unknownField":true}' 400 'simulation with unknown field'
}

$TestStarted = $false

try {
    Write-Host "Target: $BaseUrl"

    $null = Wait-BenchStatus { param($s) $s.state -eq 'READY' } 'READY'
    Write-Host 'OK: READY'

    Test-NegativeRequests

    Send-BenchCommand 'START'
    $TestStarted = $true
    $testing = (Wait-BenchStatus { param($s) $s.state -eq 'TESTING' } 'TESTING').Status
    if (-not $testing.contactorActive) { Fail 'contactorActive is not true in TESTING' }
    Write-Host 'OK: TESTING with contactorActive = true'

    $ramp = (Wait-BenchStatus { param($s) $s.state -ne 'TESTING' -or $s.pwmPercent -gt 0 } 'pwmPercent to increase').Status
    if ($ramp.state -ne 'TESTING' -or $ramp.pwmPercent -le 0) {
        Fail "pwmPercent did not increase during TESTING (state: $($ramp.state), fault: $($ramp.fault))"
    }
    Write-Host "OK: pwmPercent increased to $($ramp.pwmPercent)"

    Send-BenchCommand 'STOP'
    $stopped = Wait-BenchStatus { param($s) $s.state -eq 'STOPPED' } 'STOPPED'
    if ($stopped.Seen -contains 'SAFE_CUTOFF') {
        Write-Host 'OK: SAFE_CUTOFF observed'
    } else {
        Write-Host 'INFO: SAFE_CUTOFF too short to be observed by polling'
    }
    if ($stopped.Status.fault -ne 'NONE') { Fail "fault is $($stopped.Status.fault) after STOP, expected NONE" }
    if ($stopped.Status.pwmPercent -ne 0) { Fail "pwmPercent is $($stopped.Status.pwmPercent) in STOPPED, expected 0" }
    if ($stopped.Status.contactorActive) { Fail 'contactorActive is true in STOPPED' }
    Write-Host 'OK: STOPPED with pwmPercent = 0 and contactorActive = false'
    $TestStarted = $false

    Send-BenchCommand 'RESET'
    $reset = (Wait-BenchStatus { param($s) $s.state -in @('SELF_TEST', 'READY') } 'SELF_TEST or READY').Status
    Write-Host "OK: $($reset.state) after RESET"

    Write-Host 'PASS: simulated state machine behaves as expected'
    exit 0
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red

    if ($TestStarted) {
        # Best-effort safe state; the original error is already reported above.
        try {
            Send-BenchCommand 'STOP'
            $null = Wait-BenchStatus { param($s) $s.state -in @('STOPPED', 'FAULT') } 'STOPPED during cleanup'
            Write-Host 'Cleanup: bench stopped' -ForegroundColor Yellow
        } catch {
            Write-Warning "Cleanup failed, the bench may still be running: $($_.Exception.Message)"
        }
    }
    exit 1
}
