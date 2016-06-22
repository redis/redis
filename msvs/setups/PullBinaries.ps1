param (
    [string] $Version = $(throw 'Redis version to download is required')
)

$ErrorActionPreference = "Stop"
[Reflection.Assembly]::LoadWithPartialName("System.IO.Compression.FileSystem") | Out-Null

$ZipFilename    = "redis-x64-" + $Version + ".zip"
$CurDir         = split-path -parent $MyInvocation.MyCommand.Definition
$SourceZip      = [System.IO.Path]::Combine($CurDir, $ZipFilename )
$DestinationDir = [System.IO.Path]::Combine($CurDir, "signed_binaries" )
$GithubUrl      = "https://github.com/MSOpenTech/redis/releases/download/win-" + $Version + "/" + $ZipFilename

[System.IO.File]::Delete($SourceZip)
[System.IO.Directory]::CreateDirectory($DestinationDir) | Out-Null

ForEach( $file in [System.IO.Directory]::EnumerateFiles($DestinationDir) ) {
	[System.IO.File]::Delete($file)
}

Write-Host "Downloading zip file from $GithubUrl"
(New-Object Net.WebClient).DownloadFile($GithubUrl, $SourceZip);
Write-Host "Download complete." -foregroundcolor black -backgroundcolor green

Write-Host "Extracting files to $DestinationDir"
[System.IO.Compression.ZipFile]::ExtractToDirectory($SourceZip,$DestinationDir)
Write-Host "Extraction complete." -foregroundcolor black -backgroundcolor green

# Clean up
[System.IO.File]::Delete($SourceZip)

Write-Host "Sign the binaries and then run CreatePackages.ps1" -foregroundcolor red -backgroundcolor yellow
