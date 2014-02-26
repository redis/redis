[Reflection.Assembly]::LoadWithPartialName("System.IO.Compression.FileSystem") | Out-Null

$Compression = [System.IO.Compression.CompressionLevel]::Optimal
$IncludeBaseDirectory = $false

$CurDir = [System.IO.Directory]::GetCurrentDirectory() 
$PubDir = [System.IO.Path]::Combine($CurDir, "x64\Release\pub" )
$SourceDir = [System.IO.Path]::Combine($CurDir, "x64\Release" )
$Destination = [System.IO.Path]::Combine($CurDir, "..\bin\Release\redis-2.8.4.zip" )

[System.IO.Directory]::CreateDirectory($PubDir) | Out-Null

ForEach( $file in [System.IO.Directory]::EnumerateFiles($PubDir) ) {
	[System.IO.File]::Delete($file)
}

ForEach( $file in [System.IO.Directory]::EnumerateFiles($SourceDir, "*.exe" ) ) {
	[System.IO.File]::Copy($file, [System.IO.Path]::Combine( $PubDir, [System.IO.Path]::GetFileName($file) ) )
}

ForEach( $file in [System.IO.Directory]::EnumerateFiles($SourceDir, "*.dll" ) ) {
	[System.IO.File]::Copy($file, [System.IO.Path]::Combine( $PubDir, [System.IO.Path]::GetFileName($file) ) )
}

If ( [System.IO.File]::Exists($Destination) ) {
	[System.IO.File]::Delete($Destination)
}

[System.IO.Compression.ZipFile]::CreateFromDirectory($PubDir,$Destination,$Compression,$IncludeBaseDirectory)