[Reflection.Assembly]::LoadWithPartialName("System.IO.Compression.FileSystem") | Out-Null

$CurDir = split-path -parent $MyInvocation.MyCommand.Definition

$SourceZip = [System.IO.Path]::Combine($CurDir, "..\..\bin\Release\redis-2.8.16.zip" )
$Destination = [System.IO.Path]::Combine($CurDir, "signed_binaries" )

[System.IO.Directory]::CreateDirectory($Destination) | Out-Null

ForEach( $file in [System.IO.Directory]::EnumerateFiles($Destination) ) {
	[System.IO.File]::Delete($file)
}

[System.IO.Compression.ZipFile]::ExtractToDirectory($SourceZip,$Destination)

Write-Host "Binaries copied from $SourceZip to $Destination" -foregroundcolor black -backgroundcolor green
Write-Host "Sign these and then run CreatePackages.ps1" -foregroundcolor red -backgroundcolor yellow
