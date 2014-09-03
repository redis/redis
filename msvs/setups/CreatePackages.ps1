$CurDir = split-path -parent $MyInvocation.MyCommand.Definition

$ChocolateyDir = $CurDir + "\chocolatey"
$NugetDir = $CurDir + "\nuget"
$PackagesDir = $CurDir + "\packages"

If (Test-Path $PackagesDir){
	Remove-Item $PackagesDir -recurse | Out-Null
}
New-Item $PackagesDir -type directory  | Out-Null
New-Item ($PackagesDir+"\Chocolatey") -type directory  | Out-Null
New-Item ($PackagesDir+"\NuGet") -type directory  | Out-Null

Set-Location $ChocolateyDir
invoke-expression "chocolatey pack Redis.nuspec"
Copy-Item *.nupkg ..\packages\Chocolatey

Set-Location $NugetDir
invoke-expression "NuGet Pack Redis.nuspec"
Copy-Item *.nupkg ..\packages\NuGet

Set-Location $CurDir

Write-Host "The .nupkg files are in the 'packages' directory." -foregroundcolor black -backgroundcolor green
Write-Host "Run PushPackages to push them." -foregroundcolor red -backgroundcolor yellow
