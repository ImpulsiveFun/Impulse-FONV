[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputFile
)

$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (!(Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw 'The 64-bit .NET Framework 4.x compiler is required.'
}
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputFile)
$root = (Get-Item -LiteralPath $PSScriptRoot).Parent.FullName
$archivePath = [IO.Path]::GetTempFileName()
$extensions = @('.md', '.txt', '.c', '.h', '.inc', '.m', '.java', '.py', '.xml', '.sh', '.gradle', '.ps1', '.cs')
$topLevel = @('.gitattributes', '.gitignore', 'Build-Windows.ps1', 'BUILD.md', 'LICENSE', 'NOTICES.md', 'README.md', 'requirements.txt')
$sourceDirectories = @('.githooks', 'android', 'docs', 'ghidra_scripts', 'recomp', 'setup', 'tools', 'windows')
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
try {
    $stream = [IO.File]::Open($archivePath, [IO.FileMode]::Create)
    try {
        $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
        try {
            $files = foreach ($name in $topLevel) { Get-Item -LiteralPath (Join-Path $root $name) -Force }
            $files += foreach ($name in $sourceDirectories) {
                Get-ChildItem -LiteralPath (Join-Path $root $name) -File -Recurse -Force | Where-Object {
                    ($extensions -contains $_.Extension -or $_.Name -in @('.gitignore', 'pre-commit')) -and
                    $_.FullName.Substring($root.Length + 1) -notmatch '(^|[\\/])(\.gradle|\.cxx|build|generated|third_party|__pycache__)([\\/]|$)' -and
                    $_.Name -ne 'startup_image.h'
                }
            }
            foreach ($file in ($files | Sort-Object FullName)) {
                $entry = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
                if ($file.Length -gt 1MB -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                    throw "Unexpected release source file: $entry"
                }
                $content = [IO.File]::ReadAllText($file.FullName)
                if ($content.Contains([char]0) -or $content -match '(?i)[a-z]:[\\/]Users[\\/][^\\/]+[\\/]') {
                    throw "Unexpected local or binary content: $entry"
                }
                $zipEntry = $archive.CreateEntry($entry, [IO.Compression.CompressionLevel]::Optimal)
                $zipEntry.LastWriteTime = [DateTimeOffset]::new(2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
                $entryStream = $zipEntry.Open()
                try {
                    $inputStream = [IO.File]::OpenRead($file.FullName)
                    try { $inputStream.CopyTo($entryStream) } finally { $inputStream.Dispose() }
                } finally { $entryStream.Dispose() }
            }
        } finally { $archive.Dispose() }
    } finally { $stream.Dispose() }
    & $compiler /nologo /target:winexe /platform:x64 /optimize+ /r:System.Windows.Forms.dll /r:System.Drawing.dll `
        /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll "/resource:$archivePath,builder-source.zip" `
        "/out:$output" (Join-Path $PSScriptRoot 'Builder.cs') (Join-Path $PSScriptRoot 'ToolSetup.cs')
    if ($LASTEXITCODE -ne 0) { throw "The launcher build failed with exit code $LASTEXITCODE." }
} finally {
    Remove-Item -LiteralPath $archivePath
}
