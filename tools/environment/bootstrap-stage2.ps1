param(
    [string]$DataDirectory = 'D:\agent-io-stage2\20261110',
    [string]$CMake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Push-Location $projectRoot
try {
    function Invoke-Checked {
        param([string]$Program, [string[]]$Arguments)
        & $Program @Arguments
        if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
    }
    $dependencyRoot = Join-Path $projectRoot 'build\deps\DiskANN'
    $toolsRoot = Join-Path $projectRoot 'build\tools'
    $nuget = Join-Path $toolsRoot 'nuget.exe'
    New-Item -ItemType Directory -Force $toolsRoot | Out-Null
    if (-not (Test-Path -LiteralPath $nuget)) {
        Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/v6.12.1/nuget.exe' -OutFile $nuget
    }
    if ((Get-FileHash -LiteralPath $nuget -Algorithm SHA256).Hash -ne
        '0790BB7A0C898E44B70F2B65E3070B4DB8AF23897E38B8653D72D268B6E8BB11') {
        throw 'NuGet executable checksum mismatch'
    }
    if (-not (Test-Path -LiteralPath $dependencyRoot)) {
        Invoke-Checked git @('clone', '--depth', '1', '--branch', '0.6.1',
            'https://github.com/microsoft/DiskANN.git', $dependencyRoot)
    }
    $dependencyCommit = & git -C $dependencyRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $dependencyCommit -ne '4c31367b6c3df5395bf8b786299988a5d001ec5a') {
        throw 'DiskANN commit does not match dependency lock'
    }
    Invoke-Checked git @('-C', $dependencyRoot, 'submodule', 'update', '--init', '--depth', '1')
    $patch = Join-Path $PSScriptRoot 'diskann-0.6.1.patch'
    & git -C $dependencyRoot apply --reverse --check $patch 2>$null
    if ($LASTEXITCODE -ne 0) {
        Invoke-Checked git @('-C', $dependencyRoot, 'apply', '--check', $patch)
        Invoke-Checked git @('-C', $dependencyRoot, 'apply', $patch)
    }
    $env:PATH = "$toolsRoot;$env:PATH"
    Invoke-Checked $CMake @('-S', $dependencyRoot, '-B', 'build/diskann',
        '-G', 'Visual Studio 17 2022', '-A', 'x64',
        '-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /GR /EHsc /utf-8',
        '-DCMAKE_CONFIGURATION_TYPES=Debug;Release', '-Wno-dev')
    Invoke-Checked $CMake @('--build', 'build/diskann', '--config', 'Release',
        '--target', 'build_disk_index', 'search_disk_index', '--parallel', '4')
    Invoke-Checked $CMake @('-S', '.', '-B', 'build/windows',
        '-G', 'Visual Studio 17 2022', '-A', 'x64',
        "-DAGENT_IO_DISKANN_ROOT=$dependencyRoot",
        "-DAGENT_IO_DISKANN_PACKAGES=$projectRoot/build/diskann/packages")
    Invoke-Checked $CMake @('--build', 'build/windows', '--config', 'Release')
    # Debug checks our contracts, I/O and launcher; DiskANN is a Release dependency.
    Invoke-Checked $CMake @('--build', 'build/windows', '--config', 'Debug',
        '--target', 'agent_io_contracts', 'windows_environment_probe', 'job_runner')
    $ctest = Join-Path (Split-Path $CMake) 'ctest.exe'
    foreach ($configuration in @('Release', 'Debug')) {
        Invoke-Checked $ctest @('--test-dir', 'build/windows', '-C', $configuration, '-V')
    }
    if (-not (Test-Path -LiteralPath '.venv\Scripts\python.exe')) {
        Invoke-Checked python @('-m', 'venv', '.venv')
    }
    $python = Join-Path $projectRoot '.venv\Scripts\python.exe'
    Invoke-Checked $python @('-m', 'pip', 'install', '-r', 'evaluation/requirements.lock.txt')
    Invoke-Checked $python @('-m', 'pip', 'check')
    Invoke-Checked $python @('tools/environment/stage2.py', '--directory', $DataDirectory)
}
finally {
    Pop-Location
}
