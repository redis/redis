param (
    [string] $Version = $(throw 'Redis version to push is required')
)

Write-Host "Ensure that the api keys have been set for chocolatey and nuget" -foregroundcolor red -backgroundcolor yellow
Write-Host "  NuGet SetApiKey <your key here> -source http://chocolatey.org/" -foregroundcolor red -backgroundcolor yellow
Write-Host "  NuGet SetApiKey <your key here>" -foregroundcolor red -backgroundcolor yellow

$CurDir = split-path -parent $MyInvocation.MyCommand.Definition

$PackagesDir = $CurDir + "\packages"
$ChocolateyDir = $PackagesDir + "\Chocolatey"
$NugetDir = $PackagesDir + "\Nuget"

Set-Location $ChocolateyDir
$ChocolateyCommand = "chocolatey push Redis-64." + $Version + ".nupkg"
invoke-expression $ChocolateyCommand

Set-Location $NugetDir
$NugetCommand = "NuGet push Redis-64." + $Version + ".nupkg"
invoke-expression $NugetCommand

Set-Location $CurDir

Write-Host "The .nupkg files have been pushed!" -foregroundcolor black -backgroundcolor green
