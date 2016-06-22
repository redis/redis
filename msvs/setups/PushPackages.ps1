param (
    [string] $Version = $(throw 'Redis version to push is required')
)

$ErrorActionPreference = "Stop"

Write-Host "Ensure that the api keys have been set for chocolatey and nuget" -foregroundcolor red -backgroundcolor yellow
Write-Host "  choco apikey -k <your key here> -s https://chocolatey.org/" -foregroundcolor red -backgroundcolor yellow
Write-Host "  NuGet SetApiKey <your key here>" -foregroundcolor red -backgroundcolor yellow

$CurDir = split-path -parent $MyInvocation.MyCommand.Definition

$PackagesDir = $CurDir + "\packages"
$ChocolateyDir = $PackagesDir + "\chocolatey"
$NugetDir = $PackagesDir + "\nuget"

Set-Location $ChocolateyDir
$ChocolateyCommand = "chocolatey push Redis-64." + $Version + ".nupkg -s https://chocolatey.org/"
invoke-expression $ChocolateyCommand
if ($LASTEXITCODE -eq 0) {
    Write-Host "Chocolatey package pushed successfully." -foregroundcolor black -backgroundcolor green
} else {
    Write-Host "FAILED to push the Chocolatey package." -foregroundcolor white -backgroundcolor red
}

Set-Location $NugetDir
$NugetCommand = "NuGet push Redis-64." + $Version + ".nupkg"
invoke-expression $NugetCommand
if ($LASTEXITCODE -eq 0) {
    Write-Host "NuGet package pushed successfully" -foregroundcolor black -backgroundcolor green
} else {
    Write-Host "FAILED to push the NuGet package." -foregroundcolor white -backgroundcolor red
}

Set-Location $CurDir

