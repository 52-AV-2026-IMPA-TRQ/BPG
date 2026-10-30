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
Check 'component requires nvs_flash but not HTTP/LCD/GPIO drivers' ($Cmake -match 'nvs_flash' -and -not ($Cmake -match 'http|lcd|gpio|driver'))

$StoragePath = Join-Path $ComponentDir 'bench_profiles_storage.c'
$StorageHeader = Get-Content -Raw (Join-Path $ComponentDir 'include\bench_profiles_storage.h')
$StorageCode = [regex]::Replace((Get-Content -Raw $StoragePath), '(?s)/\*.*?\*/|//[^\r\n]*|(?m)^#include[^\r\n]*', '')
$ProdHooks = [regex]::Replace($StorageCode, '(?s)#ifdef BENCH_PROFILES_TEST_HOOKS.*?#endif', '')

Check 'storage uses namespace bpg_profiles' ($StorageHeader -match '#define\s+BENCH_PROFILES_NVS_NAMESPACE\s+"bpg_profiles"')
Check 'storage declares selected and schema keys' ($StorageHeader -match 'KEY_SELECTED\s+"selected"' -and $StorageHeader -match 'KEY_SCHEMA\s+"schema"')
Check 'storage keys slot01..slot10' ($StorageCode -match '"slot%02u"' -and $StorageCode -match 'BENCH_PROFILE_SLOT_COUNT')
Check 'storage schema version is 1' ($StorageHeader -match 'BENCH_PROFILES_STORAGE_SCHEMA_VERSION\s+1U')
Check 'storage checks CRC32 and magic' ($StorageCode -match 'esp_rom_crc32_le' -and $StorageCode -match 'BLOB_MAGIC')
Check 'storage revalidates with bench_profiles_validate' ($StorageCode -match 'bench_profiles_validate\s*\(')
Check 'storage never writes bench_profile_t as a blob' (-not ($StorageCode -match 'nvs_set_blob\s*\([^;]*sizeof\s*\(\s*(bench_profile_t|\*?profile)\s*\)'))
Check 'storage has no dynamic memory' (-not ($StorageCode -match '\b(malloc|calloc|realloc|free)\s*\('))
Check 'storage has no GPIO/LEDC/contactor' (-not ($StorageCode -match 'gpio_|ledc_|driver/|contactor_(set|on|off)\w*\s*\('))
Check 'storage never erases the whole NVS partition' (-not ($StorageCode -match 'nvs_flash_erase|nvs_flash_deinit'))
Check 'erase_all is not called by bench_profiles' (-not ($Code -match 'bench_profiles_storage_erase_all'))
Check 'production storage uses the nvs partition' ($ProdHooks -match 'NVS_DEFAULT_PART_NAME')
Check 'partition label override exists only under the test-hook macro' ($StorageCode -match '(?s)#ifdef BENCH_PROFILES_TEST_HOOKS.*test_set_partition_label' -and -not ($ProdHooks -match 'test_set_partition_label'))
Check 'operations use a FreeRTOS mutex and never hold the spinlock across NVS' ($Code -match 'xSemaphoreCreateMutexStatic' -and -not ([regex]::IsMatch($Code, '(?s)taskENTER_CRITICAL[^}]*?bench_profiles_storage_\w+\(')))
Check 'set persists before updating RAM' ($SetBody.IndexOf('bench_profiles_storage_save_slot') -ge 0 -and $SetBody.IndexOf('bench_profiles_storage_save_slot') -lt $SetBody.LastIndexOf('s_profiles[slot_index] = candidate'))

$MainSource = Get-Content -Raw (Join-Path $Root 'main\esp_rest_main.c')
$AppMain = [regex]::Match($MainSource, '(?s)void app_main\(void\).*').Value
$Order = 'bench_core_init', 'nvs_flash_init', 'bench_profiles_init', 'bench_simulator_start', 'bench_controller_start', 'esp_netif_init' |
    ForEach-Object { $AppMain.IndexOf($_) }
$InOrder = $true
for ($i = 0; $i -lt $Order.Count; $i++) {
    if ($Order[$i] -lt 0 -or ($i -gt 0 -and $Order[$i] -le $Order[$i - 1])) { $InOrder = $false }
}
Check 'app_main order: core, nvs, profiles, simulator, controller, network' $InOrder
Check 'nvs_flash_init is called once and NVS is never erased automatically' (([regex]::Matches($AppMain, 'nvs_flash_init')).Count -eq 1 -and -not ($MainSource -match 'nvs_flash_erase'))
Check 'production partition table is unchanged by this feature' (-not ((Get-Content -Raw (Join-Path $Root 'partitions_example.csv')) -match 'nvs_test'))

if ($Failures.Count -gt 0) {
    Write-Host "$($Failures.Count) check(s) failed."
    exit 1
}
Write-Host 'All static checks passed.'
