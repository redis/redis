$ErrorActionPreference = "Stop"

$CurDir            = split-path -parent $MyInvocation.MyCommand.Definition
$PackagesDir       = $CurDir + "\packages"
$ChocolateySrcDir  = $CurDir + "\chocolatey"
$ChocolateyDestDir = $PackagesDir + "\chocolatey"
$NugetSrcDir       = $CurDir + "\nuget"
$NugetDestDir      = $PackagesDir + "\nuget"

If (Test-Path $PackagesDir){
	Remove-Item $PackagesDir -recurse | Out-Null
}
New-Item $PackagesDir       -type directory  | Out-Null
New-Item $ChocolateyDestDir -type directory  | Out-Null
New-Item $NugetDestDir      -type directory  | Out-Null

Set-Location $ChocolateySrcDir
invoke-expression "chocolatey pack Redis.nuspec"
if ($LASTEXITCODE -eq 0) {
    Copy-Item *.nupkg $ChocolateyDestDir
    Write-Host "Chocolatey package copied to the destination folder." -foregroundcolor black -backgroundcolor green
} else {
    Write-Host "FAILED to create the Chocolatey package." -foregroundcolor white -backgroundcolor red
}

Set-Location $NugetSrcDir
invoke-expression "nuget pack Redis.nuspec"
if ($LASTEXITCODE -eq 0) {
    Copy-Item *.nupkg $NugetDestDir
    Write-Host "NuGet package copied to the destination folder." -foregroundcolor black -backgroundcolor green
} else {
    Write-Host "FAILED to create the NuGet package." -foregroundcolor white -backgroundcolor red
}

Write-Host "Run PushPackages to push the packages." -foregroundcolor red -backgroundcolor yellow
Set-Location $CurDir
