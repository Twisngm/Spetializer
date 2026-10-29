[CmdletBinding()]
param(
    [string]$DllPath,
    [string]$OutputDirectory,
    [ValidateRange(3, 30)][int]$BenchmarkRepeats = 5,
    [ValidateRange(16, 4096)][int]$BenchmarkBlocks = 96
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$DllPath) { $DllPath = Join-Path $projectRoot 'Assets/Plugins/x86_64/AudioPluginDemo.dll' }
$DllPath = (Resolve-Path -LiteralPath $DllPath).Path
if (!$OutputDirectory) {
    $runId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $OutputDirectory = Join-Path $PSScriptRoot "Results/$runId"
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'OutputDirectory must be new; previous measurements are never overwritten.' }
$null = New-Item -ItemType Directory -Path $OutputDirectory
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$buildDirectory = Join-Path $PSScriptRoot '.build'
$null = New-Item -ItemType Directory -Force -Path $buildDirectory
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $vswherePath)) { throw 'Visual Studio Installer/vswhere.exe is required.' }
$vsPath = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) { throw 'Install the Visual Studio C++ x64 build tools.' }
$vcvarsPath = Join-Path $vsPath 'VC/Auxiliary/Build/vcvars64.bat'
$sourcePath = Join-Path $PSScriptRoot 'SpatializerEvaluation.cpp'
$exePath = Join-Path $buildDirectory 'SpatializerEvaluation.exe'
$objectPath = Join-Path $buildDirectory 'SpatializerEvaluation.obj'
$compilerCommand = 'call "{0}" && cl /nologo /std:c++17 /O2 /fp:precise /EHsc /W4 /utf-8 /DNOMINMAX /DNDEBUG "{1}" /Fo"{2}" /Fe"{3}"' -f $vcvarsPath,$sourcePath,$objectPath,$exePath
& $env:ComSpec /d /s /c $compilerCommand 2>&1 | Tee-Object -FilePath (Join-Path $OutputDirectory 'build-output.txt')
if ($LASTEXITCODE -ne 0) { throw "Evaluation build failed: $LASTEXITCODE" }

$fileRecords = @()
foreach ($relativePath in @('Tests/SpatializerEvaluation/SpatializerEvaluation.cpp','Tests/SpatializerEvaluation/Run-Evaluation.ps1','Tests/SpatializerEvaluation/analyze_results.py','NativeCode/Plugin_Spatializer.cpp','NativeCode/SpatializerDSP.h','NativeCode/SpatializerParameters.h','NativeCode/AudioPluginInterface.h','ProjectSettings/AudioManager.asset')) {
    $absolutePath = Join-Path $projectRoot $relativePath
    if (Test-Path -LiteralPath $absolutePath) {
        $fileRecords += @{ path=$relativePath; sha256=(Get-FileHash -LiteralPath $absolutePath -Algorithm SHA256).Hash }
    }
}
$commit = & git -C $projectRoot rev-parse HEAD
$gitStatus = @(& git -C $projectRoot status --short)
$metadata = [ordered]@{
    schema_version=1; started_utc=[DateTime]::UtcNow.ToString('o'); project_root=$projectRoot
    git_commit=$commit; git_status=$gitStatus; source_files=$fileRecords
    subject='deployed_custom_spatializer_dll'; dll_path=$DllPath
    dll_sha256=(Get-FileHash -LiteralPath $DllPath -Algorithm SHA256).Hash
    dll_source_match='not_asserted; DLL hash identifies the actual tested binary'
    executable_sha256=(Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
    os=[Environment]::OSVersion.VersionString; process_architecture=$env:PROCESSOR_ARCHITECTURE
    processor=$env:PROCESSOR_IDENTIFIER; logical_processors=[Environment]::ProcessorCount
    visual_studio=$vsPath; harness_flags='/std:c++17 /O2 /fp:precise /EHsc /DNDEBUG'
    benchmark_repeats=$BenchmarkRepeats; benchmark_blocks=$BenchmarkBlocks
    benchmark_scope='single-thread offline native callback wall time; not Unity DSP CPU, audio device latency, or an underrun measurement'
    unavailable=@('Unity engine DSP CPU','Unity Native SDK original comparison','Steam Audio comparison','end-to-end device latency','custom Linear attenuation model','listening/localization scores')
}
$metadataPath = Join-Path $OutputDirectory 'metadata.json'
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8
& $exePath $DllPath $OutputDirectory $BenchmarkRepeats $BenchmarkBlocks 2>&1 | Tee-Object -FilePath (Join-Path $OutputDirectory 'execution.txt')
$evaluationExitCode = $LASTEXITCODE
$metadata['completed_utc'] = [DateTime]::UtcNow.ToString('o')
$metadata['evaluation_exit_code'] = $evaluationExitCode
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

$bundledPython = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
if (Test-Path -LiteralPath $bundledPython) { $pythonPath = $bundledPython }
else { $pythonPath = 'python' }
& $pythonPath (Join-Path $PSScriptRoot 'analyze_results.py') $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Result analysis failed.' }
Write-Output "Results: $OutputDirectory"
if ($evaluationExitCode -ne 0) { throw "Validation failed; inspect validations.csv and execution.txt ($evaluationExitCode)." }
