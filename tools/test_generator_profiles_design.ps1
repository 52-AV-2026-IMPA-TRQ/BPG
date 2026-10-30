[CmdletBinding()]
param()

# Static design checks for components/bench_profiles. They do NOT replace functional C tests.
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$ComponentDir = Join-Path $Root 'components\bench_profiles'
$Header = Get-Content -Raw (Join-Path $ComponentDir 'include\bench_profiles.h')
$SourcePath = Join-Path $ComponentDir 'bench_profiles.c'
$Source = Get-Content -Raw $SourcePath
$Cmake = Get-Content -Raw (Join-Path $ComponentDir 'CMakeLists.txt')

$Failures = New-Object System.Collections.Generic.List[string]

function Check([string]$Name, [bool]$Condition) {
    if ($Condition) {
        Write-Host "PASS: $Name"
    } else {
        Write-Host "FAIL: $Name"
        $Failures.Add($Name)
    }
}

# Strip comments so that documentation does not trigger or mask checks.
$Code = [regex]::Replace($Source, '(?s)/\*.*?\*/|//[^\r\n]*|(?m)^#include[^\r\n]*', '')

Check 'slot count is exactly 10' ($Header -match '#define\s+BENCH_PROFILE_SLOT_COUNT\s+10\b')
Check 'slot 0 means no selection' ($Header -match '#define\s+BENCH_PROFILE_NO_SELECTION_SLOT\s+0U')
Check 'initial selection is no selection' ($Code -match 'static\s+uint8_t\s+s_selected_slot_number\s*=\s*BENCH_PROFILE_NO_SELECTION_SLOT')
Check 'init creates only EMPTY profiles' ($Code -match 'make_empty_profile\(\(uint8_t\)\(slot_index \+ 1U\)\)')
Check 'no hardcoded part number or designation literals' (-not ($Code -match 'strcpy|strncpy|snprintf|\.part_number\s*=|\.designation\s*=|"[A-Za-z0-9]'))
Check 'no dynamic memory in bench_profiles' (-not ($Code -match '\b(malloc|calloc|realloc|free)\s*\('))
Check 'no GPIO/LEDC/contactor/PWM actuation' (-not ($Code -match 'gpio_|ledc_|driver/|board_pins|bench_core_(send|publish)|contactor_(set|on|off)\w*\s*\('))
$SetBody = [regex]::Match($Code, '(?s)esp_err_t bench_profiles_set\(.*?\n\}').Value
Check 'set delegates EMPTY to bench_profiles_clear' ($SetBody -match 'slot_state\s*==\s*BENCH_PROFILE_SLOT_EMPTY\s*\)\s*\{\s*return\s+bench_profiles_clear\(slot_number\)')
Check 'set handles EMPTY before copying caller data' ($SetBody.IndexOf('bench_profiles_clear') -ge 0 -and $SetBody.IndexOf('bench_profiles_clear') -lt $SetBody.IndexOf('*profile;'))
Check 'clear stores make_empty_profile' ($Code -match '(?s)esp_err_t bench_profiles_clear\(.*?make_empty_profile\(slot_number\)')
Check 'set checks initialization before validating' ($SetBody.IndexOf('is_initialized()') -ge 0 -and $SetBody.IndexOf('is_initialized()') -lt $SetBody.IndexOf('bench_profiles_validate'))
Check 'selected-slot getter returns slot 0 before init' ($Code -match 's_initialized \? s_selected_slot_number : BENCH_PROFILE_NO_SELECTION_SLOT')
Check 'uses bench_config_validate' ($Code -match '\bbench_config_validate\s*\(')
Check 'does not duplicate config limits' (-not ($Code -match 'maximum_pwm_percent\s*[<>]'))
Check 'protects state with a spinlock' ($Code -match 'portMUX_TYPE' -and $Code -match 'taskENTER_CRITICAL')
Check 'revision saturates at UINT32_MAX' ($Code -match 'UINT32_MAX')
Check 'component does not require NVS/HTTP/LCD' (-not ($Cmake -match 'nvs|http|lcd|gpio|driver'))

if ($Failures.Count -gt 0) {
    Write-Host "$($Failures.Count) check(s) failed."
    exit 1
}
Write-Host 'All static checks passed.'
