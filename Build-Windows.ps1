[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GamePath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Parameter(Mandatory = $true)][string]$GradleJavaHome,
    [string]$GhidraHeadless,
    [string]$GhidraJavaHome,
    [string]$AndroidSdk = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$Executable,
    [string]$GeneratedPath,
    [string]$PythonCommand = 'python',
    [string]$GradleCommand = 'gradle',
    [switch]$Offline,
    [switch]$SkipGameZip,
    [switch]$ShowBuildStages
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$game = (Resolve-Path -LiteralPath $GamePath).Path
$sdk = (Resolve-Path -LiteralPath $AndroidSdk).Path
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
$exe = if ($Executable) { (Resolve-Path -LiteralPath $Executable).Path } else { Join-Path $game 'FalloutNV.exe' }
$generated = if ($GeneratedPath) {
    (Resolve-Path -LiteralPath $GeneratedPath).Path
} else {
    Join-Path $output 'generated'
}

function Test-InDirectory([string]$Path, [string]$Directory) {
    $pathPrefix = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/') + '\'
    $directoryPrefix = [IO.Path]::GetFullPath($Directory).TrimEnd('\', '/') + '\'
    return $pathPrefix.StartsWith($directoryPrefix, [StringComparison]::OrdinalIgnoreCase)
}

function Invoke-Checked([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE." }
}

function Write-BuildStage([string]$Message) {
    if ($ShowBuildStages) {
        Write-Host "FNV_BUILD_STAGE: $Message"
    } else {
        Write-Host $Message
    }
}

if (!(Test-Path -LiteralPath $game -PathType Container)) { throw 'GamePath must be an installed game directory.' }
if ((Test-InDirectory $output $repo) -or (Test-InDirectory $output $game) -or
    (Test-InDirectory $generated $repo) -or (Test-InDirectory $generated $game)) {
    throw 'Use private output and generated directories outside the source and game folders.'
}
foreach ($path in @($exe, "$GradleJavaHome\bin\java.exe", "$sdk\platforms\android-35\android.jar",
        "$sdk\ndk\27.2.12479018\source.properties", "$sdk\cmake\3.22.1\bin\cmake.exe")) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing required file. $path" }
}
$PythonCommand = (Get-Command $PythonCommand -CommandType Application, ExternalScript | Select-Object -First 1).Source
$GradleCommand = (Get-Command $GradleCommand -CommandType Application, ExternalScript | Select-Object -First 1).Source
$apk = Join-Path $output 'New-Vegas-Playable.apk'
$zip = Join-Path $output 'NewVegas-Private.zip'
if (!$SkipGameZip -and (Test-Path -LiteralPath $zip)) {
    throw 'The private game ZIP already exists. Choose a new OutputPath or use -SkipGameZip to keep it.'
}

$image = Join-Path $generated 'startup_image.h'
$sha256 = [Security.Cryptography.SHA256]::Create()
$exeStream = [IO.File]::OpenRead($exe)
try {
    $sha = [BitConverter]::ToString($sha256.ComputeHash($exeStream)).Replace('-', '').ToLowerInvariant()
} finally {
    $exeStream.Dispose()
    $sha256.Dispose()
}
if ($sha -ne '19406942e48724d797300c4ea6be9ac69a32f670b8c35db09279a2258422739f') {
    Write-Warning 'This executable differs from the one tested with v150.'
}
if (Test-Path -LiteralPath $generated) {
    $recordedSha = if (Test-Path -LiteralPath $image -PathType Leaf) {
        [regex]::Match((Get-Content -LiteralPath $image -Raw),
            '(?m)^#define STARTUP_SHA256 "([0-9a-fA-F]{64})"\s*$').Groups[1].Value
    }
    if ($recordedSha -ne $sha) {
        throw 'The generated folder is incomplete or belongs to a different executable. Use a fresh output directory.'
    }
} else {
    if (!$GhidraHeadless -or !(Test-Path -LiteralPath $GhidraHeadless -PathType Leaf) -or
        !$GhidraJavaHome -or !(Test-Path -LiteralPath "$GhidraJavaHome\bin\java.exe" -PathType Leaf)) {
        throw 'Supply -GhidraHeadless and -GhidraJavaHome to generate the native build input.'
    }
    $GhidraHeadless = (Resolve-Path -LiteralPath $GhidraHeadless).Path
    $GhidraJavaHome = (Resolve-Path -LiteralPath $GhidraJavaHome).Path
}
$GradleJavaHome = (Resolve-Path -LiteralPath $GradleJavaHome).Path

$oldJava = $env:JAVA_HOME
$oldSdk = $env:ANDROID_HOME
$oldSdkRoot = $env:ANDROID_SDK_ROOT
$oldPath = $env:Path
New-Item -ItemType Directory -Path $output -Force | Out-Null
Push-Location $repo
try {
    if ($Offline) {
        foreach ($codec in @('libogg-1.3.6', 'libvorbis-1.3.7')) {
            if (!(Test-Path -LiteralPath "third_party/$codec/CMakeLists.txt" -PathType Leaf)) {
                throw "Cached-only mode needs $codec. In the builder, open Tools, clear 'Use cached dependencies only', and build again with internet access. For a command-line build, omit -Offline."
            }
        }
    } else {
        Write-BuildStage 'Checking audio codec dependencies.'
        Invoke-Checked $PythonCommand @('-B', 'tools/get_codecs.py')
    }
    if (!(Test-Path -LiteralPath $image)) {
        Write-BuildStage 'Analyzing and translating the game executable.'
        $env:JAVA_HOME = $GhidraJavaHome
        $env:Path = "$GhidraJavaHome\bin;$oldPath"
        Invoke-Checked $PythonCommand @('-B', 'android/prepare.py', $game, '--exe', $exe, '--ghidra', $GhidraHeadless,
            '--output', $generated, '--startup-group', '--virtual-tables', '--code-pointers',
            '--startup-roots', 'android/startup_roots.txt')
    } else {
        Write-Host "Using the local translation in $generated"
    }
    $env:JAVA_HOME = $GradleJavaHome
    $env:Path = "$GradleJavaHome\bin;$oldPath"
    $env:ANDROID_HOME = $sdk
    $env:ANDROID_SDK_ROOT = $sdk
    $gradleArgs = @('--no-daemon', '-p', 'android', 'assembleDebug', "-PfnvGenerated=$generated",
        '-PfnvNativeTls=true', '-PfnvFixedMemory=true')
    if ($Offline) { $gradleArgs += '--offline' }
    Write-BuildStage 'Compiling and packaging the Android APK.'
    Invoke-Checked $GradleCommand $gradleArgs
    Write-BuildStage 'Copying the APK to your output folder.'
    Copy-Item -LiteralPath 'android/build/outputs/apk/debug/NewVegasRecompProbe-debug.apk' -Destination $apk -Force
    Write-Host "Playable APK: $apk"
    if (!$SkipGameZip) {
        Write-BuildStage 'Creating the private game ZIP.'
        Invoke-Checked $PythonCommand @('-B', '-m', 'tools.make_game_zip', $game, $zip, '--exe', $exe)
        Write-Host "Private game ZIP: $zip"
    }
} finally {
    Pop-Location
    $env:JAVA_HOME = $oldJava
    $env:ANDROID_HOME = $oldSdk
    $env:ANDROID_SDK_ROOT = $oldSdkRoot
    $env:Path = $oldPath
}
