param(
    [string]$DataDirectory = 'D:\agent-io-stage2\20261110',
    [string]$CMake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    [string]$Python = ''
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
    function Test-PatchApplied {
        param([string]$Repository, [string]$Patch)
        # PowerShell 5.1 treats native stderr as an error during redirection.
        $ErrorActionPreference = 'Continue'
        & git -C $Repository apply --reverse --check $Patch 2>$null
        return $LASTEXITCODE -eq 0
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
    if (-not (Test-PatchApplied $dependencyRoot $patch)) {
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
    $venvPython = Join-Path $projectRoot '.venv\Scripts\python.exe'
    $versionCheck = 'import sys; sys.exit(0 if sys.version_info[:2] == (3, 12) else 1)'
    if (-not (Test-Path -LiteralPath $venvPython)) {
        if ($Python) {
            Invoke-Checked $Python @('-c', $versionCheck)
            Invoke-Checked $Python @('-m', 'venv', '.venv')
        } else {
            Invoke-Checked py @('-3.12', '-m', 'venv', '.venv')
        }
    }
    & $venvPython -c $versionCheck
    if ($LASTEXITCODE -ne 0) {
        throw 'The project .venv must use Python 3.12. Move the existing .venv aside and run again.'
    }
    Invoke-Checked $venvPython @('-m', 'pip', 'install', '--only-binary=:all:', '-r',
        'evaluation/requirements.lock.txt')
    Invoke-Checked $venvPython @('-m', 'pip', 'check')
    Invoke-Checked $CMake @('-S', '.', '-B', 'build/windows',
        '-G', 'Visual Studio 17 2022', '-A', 'x64',
        "-DAGENT_IO_DISKANN_ROOT=$dependencyRoot",
        "-DAGENT_IO_DISKANN_PACKAGES=$projectRoot/build/diskann/packages",
        "-DAGENT_IO_BOOST_INCLUDE=$projectRoot/build/diskann/packages/boost/lib/native/include",
        "-DAGENT_IO_TEST_PYTHON=$venvPython")
    $ctest = Join-Path (Split-Path $CMake) 'ctest.exe'
    foreach ($configuration in @('Release', 'Debug')) {
        Invoke-Checked $CMake @('--build', 'build/windows', '--config', $configuration,
            '--parallel', '4')
        Invoke-Checked $ctest @('--test-dir', 'build/windows', '-C', $configuration, '-V')
    }
    Invoke-Checked $venvPython @('tools/environment/stage2.py', '--directory', $DataDirectory)
}
finally {
    Pop-Location
}
