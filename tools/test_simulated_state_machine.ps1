[CmdletBinding()]
param(
    [string]$BaseUrl = 'http://dashboard.local',
    [int]$TimeoutSeconds = 15
)

# Simulated state machine check: only uses the REST diagnostic API, no hardware assumptions.
$ErrorActionPreference = 'Stop'
$BaseUrl = $BaseUrl.TrimEnd('/')

# The timeout scenario spans settle time + maximum test time + safe cutoff + network margin.
$TimeoutScenarioSeconds = 30

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
function Wait-BenchStatus([scriptblock]$Predicate, [string]$Description, [int]$Timeout = $TimeoutSeconds) {
    $deadline = (Get-Date).AddSeconds($Timeout)
    $seen = New-Object System.Collections.Generic.List[string]
    $errorCount = 0
    $lastError = $null
    $lastState = $null
    do {
        try {
            $status = Get-BenchStatus
        } catch {
            # A transient HTTP error or timeout is not a scenario failure by itself.
            $errorCount++
            $lastError = $_.Exception.Message
            Start-Sleep -Milliseconds 100
            continue
        }

        $lastState = $status.state
        if ($seen.Count -eq 0 -or $seen[$seen.Count - 1] -ne $status.state) {
            $seen.Add($status.state)
        }
        if (& $Predicate $status) {
            return [pscustomobject]@{ Status = $status; Seen = $seen }
        }
        Start-Sleep -Milliseconds 50
    } while ((Get-Date) -lt $deadline)

    try {
        $lastState = (Get-BenchStatus).state
    } catch {
        $errorCount++
        $lastError = $_.Exception.Message
    }

    if ($null -eq $lastState) { $lastState = 'unknown (no status could be read)' }
    if ($null -eq $lastError) { $lastError = 'none' }
    Fail "timeout waiting for $Description (last known state: $lastState, seen: $($seen -join ' -> '), HTTP errors: $errorCount, last HTTP error: $lastError)"
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

# POSTs JSON to the simulation endpoint; only the provided fields are changed.
function Set-Simulation([hashtable]$Fields) {
    $body = $Fields | ConvertTo-Json -Compress
    try {
        $null = Invoke-RestMethod -Uri "$BaseUrl/api/v1/bench/simulation" -Method Post `
            -ContentType 'application/json' -Body $body -TimeoutSec 5
    } catch {
        Fail "simulation update $body failed: $($_.Exception.Message)"
    }
    Write-Host "Simulation: $body"
}

function Set-SafeSimulation {
    Set-Simulation @{ fc1Safe = $true; fc2TorqueReached = $false; autoCurrent = $true }
}

function Wait-Ready {
    $null = Wait-BenchStatus { param($s) $s.state -eq 'READY' } 'READY'
    Write-Host 'OK: READY'
}

function Assert-SafeOutputs($Status, [string]$Context) {
    if ($Status.pwmPercent -ne 0) { Fail "pwmPercent is $($Status.pwmPercent) $Context, expected 0" }
    if ($Status.contactorActive) { Fail "contactorActive is true $Context, expected false" }
}

# RESET, then SELF_TEST or READY, then finally READY.
function Reset-ToReady {
    Send-BenchCommand 'RESET'
    $reset = (Wait-BenchStatus { param($s) $s.state -in @('SELF_TEST', 'READY') } 'SELF_TEST or READY').Status
    Write-Host "OK: $($reset.state) after RESET"
    Wait-Ready
    Assert-SafeOutputs (Get-BenchStatus) 'in READY after RESET'
}

# START, then TESTING with the contactor active and a rising PWM.
function Start-Test {
    Send-BenchCommand 'START'
    $testing = (Wait-BenchStatus { param($s) $s.state -eq 'TESTING' } 'TESTING').Status
    if (-not $testing.contactorActive) { Fail 'contactorActive is not true in TESTING' }
    Write-Host 'OK: TESTING with contactorActive = true'

    $ramp = (Wait-BenchStatus { param($s) $s.state -ne 'TESTING' -or $s.pwmPercent -gt 0 } 'pwmPercent to increase').Status
    if ($ramp.state -ne 'TESTING' -or $ramp.pwmPercent -le 0) {
        Fail "pwmPercent did not increase during TESTING (state: $($ramp.state), fault: $($ramp.fault))"
    }
    Write-Host "OK: pwmPercent increased to $($ramp.pwmPercent)"
}

# Best-effort safe condition; never throws so it cannot hide the original error.
function Cleanup-ActiveTest {
    try {
        $status = Get-BenchStatus
        if ($status.state -in @('TESTING', 'SAFE_CUTOFF')) {
            Send-BenchCommand 'STOP'
            $null = Wait-BenchStatus { param($s) $s.state -notin @('TESTING', 'SAFE_CUTOFF') } 'end of test during cleanup' 10
            Write-Host 'Cleanup: test stopped' -ForegroundColor Yellow
        }
    } catch {
        Write-Warning "Cleanup could not stop the test, the bench may still be running: $($_.Exception.Message)"
    }

    try {
        Set-SafeSimulation
    } catch {
        Write-Warning "Cleanup could not restore simulated inputs: $($_.Exception.Message)"
    }
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
    if ($s.state -ne 'READY') { Fail "state is $($s.state) after '$Description', expected READY" }
    Assert-SafeOutputs $s "after '$Description'"
    Write-Host "OK: $Description -> HTTP $Expected"
}

function Test-NegativeRequests {
    Assert-PostStatus '/api/v1/bench/command' '{"command":"INVALID"}' 400 'command INVALID'
    Assert-PostStatus '/api/v1/bench/command' '{"command":"STOP","extra":1}' 400 'command with extra field'
    Assert-PostStatus '/api/v1/bench/simulation' '{"currentA":-1}' 400 'simulation with negative currentA'
    Assert-PostStatus '/api/v1/bench/simulation' '{"unknownField":true}' 400 'simulation with unknown field'
    Assert-PostStatus '/api/v1/bench/simulation' '{"currentA":5.0,"autoCurrent":true}' 400 'simulation with currentA and autoCurrent'
}

function Test-ManualStop {
    Start-Test

    Send-BenchCommand 'STOP'
    $stopped = Wait-BenchStatus { param($s) $s.state -eq 'STOPPED' } 'STOPPED'
    if ($stopped.Seen -contains 'SAFE_CUTOFF') {
        Write-Host 'OK: SAFE_CUTOFF observed'
    } else {
        Write-Host 'INFO: SAFE_CUTOFF too short to be observed by polling'
    }
    if ($stopped.Status.fault -ne 'NONE') { Fail "fault is $($stopped.Status.fault) after STOP, expected NONE" }
    Assert-SafeOutputs $stopped.Status 'in STOPPED'
    Write-Host 'OK: STOPPED with pwmPercent = 0 and contactorActive = false'

    Reset-ToReady
}

function Test-Fc2Torque {
    Start-Test

    Set-Simulation @{ fc2TorqueReached = $true }
    $completed = (Wait-BenchStatus { param($s) $s.state -eq 'COMPLETED' } 'COMPLETED').Status
    if ($completed.fault -ne 'NONE') { Fail "fault is $($completed.fault) in COMPLETED, expected NONE" }
    if (-not $completed.fc2TorqueReached) { Fail 'fc2TorqueReached is not true in COMPLETED' }
    Assert-SafeOutputs $completed 'in COMPLETED'
    Write-Host 'OK: COMPLETED with fault NONE, pwmPercent = 0 and contactorActive = false'

    $null = Wait-BenchStatus { param($s) $s.state -eq 'COOLDOWN' } 'COOLDOWN'
    Write-Host 'OK: COOLDOWN'
    Set-Simulation @{ fc2TorqueReached = $false }

    $null = Wait-BenchStatus { param($s) $s.state -in @('SELF_TEST', 'READY') } 'SELF_TEST after COOLDOWN'
    Write-Host 'OK: SELF_TEST/READY after COOLDOWN'
    Wait-Ready
    Assert-SafeOutputs (Get-BenchStatus) 'in READY after COOLDOWN'
}

function Test-Timeout {
    Send-BenchCommand 'START'
    $fault = (Wait-BenchStatus { param($s) $s.state -eq 'FAULT' } 'FAULT by timeout' $TimeoutScenarioSeconds).Status
    if ($fault.fault -ne 'TIMEOUT') { Fail "fault is $($fault.fault), expected TIMEOUT" }
    Assert-SafeOutputs $fault 'in FAULT'
    if (-not $fault.faultLedActive) { Fail 'faultLedActive is not true in FAULT' }
    Write-Host 'OK: FAULT/TIMEOUT with pwmPercent = 0, contactorActive = false and faultLedActive = true'

    Reset-ToReady
}

function Test-Fc1PriorityOverStop {
    Start-Test

    # FC1 loss and STOP are sent back to back; FC1 must win.
    Set-Simulation @{ fc1Safe = $false }
    Send-BenchCommand 'STOP'

    $result = Wait-BenchStatus { param($s) $s.state -in @('FAULT', 'STOPPED') } 'FAULT after FC1 loss and STOP'
    $final = $result.Status
    if ($final.state -ne 'FAULT') { Fail "final state is $($final.state), expected FAULT (STOP must not override FC1 loss)" }
    if ($final.fault -ne 'FC1_SECURITY') { Fail "fault is $($final.fault), expected FC1_SECURITY" }
    Assert-SafeOutputs $final 'in FAULT'
    Write-Host 'OK: FAULT/FC1_SECURITY with pwmPercent = 0 and contactorActive = false'

    Set-Simulation @{ fc1Safe = $true }
    Reset-ToReady
}

function Invoke-Scenario([string]$Name, [scriptblock]$Body) {
    Write-Host ''
    Write-Host "=== Scenario: $Name ===" -ForegroundColor Cyan
    Set-SafeSimulation
    Wait-Ready
    & $Body
    Write-Host "PASS: $Name"
}

try {
    Write-Host "Target: $BaseUrl"

    Invoke-Scenario 'Negative requests' { Test-NegativeRequests }
    Invoke-Scenario '1. Manual STOP' { Test-ManualStop }
    Invoke-Scenario '2. Torque reached (FC2)' { Test-Fc2Torque }
    Invoke-Scenario '3. Maximum test time (TIMEOUT)' { Test-Timeout }
    Invoke-Scenario '4. FC1 loss has priority over STOP' { Test-Fc1PriorityOverStop }

    Write-Host ''
    Write-Host 'SKIP: OVERCURRENT requires a validated non-zero current limit'
    Write-Host 'PASS: simulated state machine behaves as expected'
    exit 0
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    Cleanup-ActiveTest
    exit 1
}
