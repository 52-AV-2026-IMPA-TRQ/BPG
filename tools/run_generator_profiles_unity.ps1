<#
.SYNOPSIS
    Builds, flashes and monitors the Unity tests of components/bench_profiles on an ESP32.

.DESCRIPTION
    Generates a temporary Unity application OUTSIDE the repository
    ($env:TEMP\bpg_generator_profiles_unity) that only includes bench_core,
    bench_profiles and its test directory. No versioned file is modified.

    Actions:
      Build   - recreate the temporary app and compile it (default). Does not flash.
      Flash   - write ONLY the Unity app at 0x10000 (replaces BPG in the factory partition).
      Monitor - open the serial monitor for the Unity app.
      Restore - write ONLY build\BPG.bin at 0x10000 to bring the normal firmware back.
      Clean   - delete the temporary folder only.

    Requires an ESP-IDF environment (IDF_PATH set, idf.py and python in PATH).
#>
[CmdletBinding()]
param(
    [ValidateSet('Build', 'Flash', 'Monitor', 'Restore', 'Clean')]
    [string]$Action = 'Build',

    [string]$Port = 'COM3',

    [int]$Baud = 115200
)

$ErrorActionPreference = 'Stop'

$ExpectedTestCount = 12
$AppAddress = '0x10000'
$ProjectName = 'bpg_generator_profiles_unity'

function Fail([string]$Message) {
    Write-Host "ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Get-RepositoryRoot {
    $candidate = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
    $required = @('components\bench_core', 'components\bench_profiles\test', 'main')
    foreach ($relative in $required) {
        if (-not (Test-Path (Join-Path $candidate $relative))) {
            Fail "'$candidate' does not look like the BPG repository (missing $relative)."
        }
    }
    return $candidate
}

function ConvertTo-CMakePath([string]$Path) {
    return ([IO.Path]::GetFullPath($Path)).Replace('\', '/')
}

function Write-TextFile([string]$Path, [string]$Content) {
    $directory = Split-Path -Parent $Path
    if (-not (Test-Path $directory)) {
        New-Item -ItemType Directory -Path $directory | Out-Null
    }
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($Path, $Content, $utf8NoBom)
}

function Assert-IdfEnvironment {
    if ([string]::IsNullOrWhiteSpace($env:IDF_PATH)) {
        Fail 'IDF_PATH is not defined. Open an ESP-IDF PowerShell environment first.'
    }
    if (-not (Test-Path $env:IDF_PATH)) {
        Fail "IDF_PATH points to a missing folder: $($env:IDF_PATH)"
    }
}

function Assert-PythonAndEsptool {
    if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
        Fail 'python was not found in PATH.'
    }
    & python -m esptool version *> $null
    if ($LASTEXITCODE -ne 0) {
        Fail 'esptool is not available (python -m esptool failed).'
    }
}

function Assert-IdfPy {
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
        Fail 'idf.py was not found in PATH.'
    }
}

function Assert-PortProvided {
    if ([string]::IsNullOrWhiteSpace($Port)) {
        Fail 'Port is empty. Use -Port COMx.'
    }
}

function Show-DownloadModeInstructions {
    Write-Host ''
    Write-Host 'Put the board in download mode MANUALLY:'
    Write-Host '  1. Hold BOOT.'
    Write-Host '  2. Press and release EN.'
    Write-Host '  3. Release BOOT.'
    Write-Host ''
    Read-Host 'Press Enter when the board is in download mode' | Out-Null
}

function Invoke-EsptoolWrite([string]$BinaryPath) {
    & python -m esptool --chip esp32 --port $Port --baud $Baud `
        --before no-reset --after hard-reset --no-stub `
        write-flash $AppAddress $BinaryPath
    if ($LASTEXITCODE -ne 0) {
        Fail "esptool failed with exit code $LASTEXITCODE."
    }
}

$RepositoryRoot = Get-RepositoryRoot
$TempRoot = Join-Path ([IO.Path]::GetFullPath($env:TEMP)) 'bpg_generator_profiles_unity'
$BuildDir = Join-Path $TempRoot 'build'
$AppBinary = Join-Path $BuildDir "$ProjectName.bin"
$FlashArgsPath = Join-Path $BuildDir 'flash_args'

# Never operate on anything but the expected folder name, and never inside the repository.
if ((Split-Path -Leaf $TempRoot) -ne 'bpg_generator_profiles_unity' -or
    $TempRoot.StartsWith($RepositoryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    Fail "Unsafe temporary path: $TempRoot"
}

Write-Host "Action: $Action"
Write-Host "Repository: $RepositoryRoot"
Write-Host "Temporary Unity app: $TempRoot"

function New-UnityApp {
    if (Test-Path $TempRoot) {
        Remove-Item -Recurse -Force $TempRoot
    }
    New-Item -ItemType Directory -Path (Join-Path $TempRoot 'main') | Out-Null

    $coreDir = ConvertTo-CMakePath (Join-Path $RepositoryRoot 'components\bench_core')
    $profilesDir = ConvertTo-CMakePath (Join-Path $RepositoryRoot 'components\bench_profiles')
    $testDir = ConvertTo-CMakePath (Join-Path $RepositoryRoot 'components\bench_profiles\test')

    $rootCmake = @"
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS
    "$coreDir"
    "$profilesDir"
    "$testDir")
include(`$ENV{IDF_PATH}/tools/cmake/project.cmake)
project($ProjectName)
"@

    # The test component is linked whole-archive so its TEST_CASE constructors are kept.
    $mainCmake = @'
idf_component_register(
    SRCS "test_app_main.c"
    REQUIRES unity test
)
idf_component_set_property(test WHOLE_ARCHIVE TRUE)
'@

    $mainSource = @'
#include <stdio.h>

#include "unity.h"
#include "unity_test_runner.h"

void app_main(void)
{
    int registered_tests = 0;
    test_desc_t info;
    while (unity_get_test_info(registered_tests, &info)) {
        registered_tests++;
    }

    printf("\n=== BPG generator profiles Unity tests (temporary app) ===\n");
    printf("Registered tests: %d\n", registered_tests);

    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();

    printf("Automatic run finished. Unity menu available:\n");
    unity_run_menu();
}
'@

    Write-TextFile (Join-Path $TempRoot 'CMakeLists.txt') $rootCmake
    Write-TextFile (Join-Path $TempRoot 'main\CMakeLists.txt') $mainCmake
    Write-TextFile (Join-Path $TempRoot 'main\test_app_main.c') $mainSource
}

function Assert-AppBuilt {
    if (-not (Test-Path $AppBinary)) {
        Fail "Unity application is not built ($AppBinary missing). Run -Action Build first."
    }
}

switch ($Action) {
    'Build' {
        Assert-IdfEnvironment
        Assert-IdfPy

        New-UnityApp

        & idf.py -C $TempRoot -B $BuildDir set-target esp32
        if ($LASTEXITCODE -ne 0) { Fail "idf.py set-target failed ($LASTEXITCODE)." }

        & idf.py -C $TempRoot -B $BuildDir build
        if ($LASTEXITCODE -ne 0) { Fail "idf.py build failed ($LASTEXITCODE)." }

        if (-not (Test-Path $AppBinary)) { Fail "Application binary not found: $AppBinary" }

        # The application address comes from flash_args when available; 0x10000 is the validated value.
        $address = $AppAddress
        if (Test-Path $FlashArgsPath) {
            $match = Select-String -Path $FlashArgsPath -Pattern "^(0x[0-9a-fA-F]+)\s+$([regex]::Escape($ProjectName))\.bin" |
                Select-Object -First 1
            if ($match) { $address = $match.Matches[0].Groups[1].Value }
        }
        if ([Convert]::ToInt32($address, 16) -ne [Convert]::ToInt32($AppAddress, 16)) {
            Fail "Unexpected application address $address (expected $AppAddress)."
        }

        $size = (Get-Item $AppBinary).Length
        Write-Host ''
        Write-Host "Binary:  $AppBinary"
        Write-Host "Size:    $size bytes"
        Write-Host "Address: $address"
        Write-Host 'Build complete. The board was NOT flashed.'
    }

    'Flash' {
        Assert-IdfEnvironment
        Assert-PythonAndEsptool
        Assert-PortProvided
        Assert-AppBuilt

        Write-Host ''
        Write-Host 'La aplicación Unity sustituirá temporalmente BPG en la partición factory.' -ForegroundColor Yellow
        Write-Host "Only $AppAddress is written; bootloader, partition table and LittleFS are untouched."
        Show-DownloadModeInstructions
        Invoke-EsptoolWrite $AppBinary
        Write-Host 'Flash complete. Run -Action Monitor to see the test results.'
    }

    'Monitor' {
        Assert-IdfEnvironment
        Assert-IdfPy
        Assert-PortProvided
        Assert-AppBuilt

        Write-Host ''
        Write-Host 'The tests run automatically at boot.'
        Write-Host 'Afterwards the Unity menu stays available.'
        Write-Host 'Press Ctrl+T followed by X to exit the monitor.'
        Write-Host ''
        & idf.py -C $TempRoot -B $BuildDir -p $Port monitor
        if ($LASTEXITCODE -ne 0) { Fail "idf.py monitor failed ($LASTEXITCODE)." }
    }

    'Restore' {
        Assert-IdfEnvironment
        Assert-PythonAndEsptool
        Assert-PortProvided

        $bpgBinary = Join-Path $RepositoryRoot 'build\BPG.bin'
        if (-not (Test-Path $bpgBinary)) {
            Fail "$bpgBinary not found. Build BPG first with idf.py build."
        }

        Write-Host ''
        Write-Host 'This restores the normal BPG firmware.' -ForegroundColor Yellow
        Write-Host "Only $AppAddress is written; www.bin is not rewritten (LittleFS was not modified)."
        Show-DownloadModeInstructions
        Invoke-EsptoolWrite $bpgBinary

        Write-Host ''
        Write-Host 'To verify BPG is running again:'
        Write-Host '  Invoke-RestMethod -Uri "http://dashboard.local/api/v1/bench/status"'
    }

    'Clean' {
        if (Test-Path $TempRoot) {
            Remove-Item -Recurse -Force $TempRoot
            Write-Host 'Temporary folder removed.'
        } else {
            Write-Host 'Nothing to clean.'
        }
    }
}
