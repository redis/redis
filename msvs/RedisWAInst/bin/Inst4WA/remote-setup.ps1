<#
   Copyright © Microsoft Open Technologies, Inc.
   All Rights Reserved
   Apache 2.0 License

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.
#>

Add-Type -AssemblyName System.ServiceModel.Web, System.Runtime.Serialization
$remotesetup1file = $args[0]
$mylocation =  [System.IO.Path]::GetDirectoryName($MyInvocation.MyCommand.Path)
Set-Location $mylocation

function logStatus {
    param ($message)
    Write-Host "info:   $message" -foregroundcolor "yellow"
}

function logStatus2 {
    param ($message)
    Write-Host "info:   $message" -foregroundcolor "blue"
}

function logErr {
    param ($message)
    Write-Host "error:  $message" -foregroundcolor "red"
}

function logSuccess {
    param ($message)
    Write-Host "info:   $message" -foregroundcolor "green"
}

function logInput {
    param ($message)
    Write-Host "input:  $message" -foregroundcolor "magenta"
}

<#
    Display the usage
#>
function showUsageAndExit() {
    Write-Host "`r`nUSAGE: remote-setup.ps1 <OSSInstallScript> <Service> <Admin-Password> <WinRM-EndPointName-Prefix> `r`n" -foregroundcolor "green"
    exit 1
}

# Gets the commandline arguments
if($args.Count -ne 4)
{
    logErr "Missing required arguments`r`n"
    showUsageAndExit
}


<#
    Reads the configuration file config.json 
#>
function get-Config([string]$service, [string]$adminPassword, [string]$winrmEpName)
{
   
        [hashtable]$result = @{} 
        $result.dnsPrefix = $service
        $result.dns = $service + ".cloudapp.net"
        $result.user = "Administrator"
        $result.password = $adminPassword
       
        $result.ports = @();
        $result.ips = @();
        
      
        $vmDetails = @(Get-AzureVM -ServiceName $service | select-object *)
        if($vmDetails.length -le 0)
        {
            Throw "Unable to get VM details."
        }

        $endpointSearchCriteria = "*" + $winrmEpName + "*"

        foreach($eachVmDetail in $vmDetails)
        {
            $endpoints = @(Get-AzureVM -ServiceName $service -name $eachVmDetail.Name | Get-AzureEndPoint | where-object {$_.Name -like $endpointSearchCriteria })
            $result.ports += $endpoints[0].Port
            $result.ips += $eachVmDetail.IpAddress
        }
        
        return $result;
   
}

logStatus "Reading the configuration file.."
$result = get-Config $args[1] $args[2] $args[3]
logStatus ("DNS: " + $result.Get_Item("dns"))
logStatus ("VM IP Addresses:" + $result.Get_Item("ips"))
$secpasswd = ConvertTo-SecureString $result.Get_Item("password") -AsPlainText -Force
$cred = New-Object System.Management.Automation.PSCredential ($result.Get_Item("user"), $secpasswd)
# After VM's are in ready state it takes some time to reach the VM in a state in which
# it can accept the request, so waiting for 30 seconds
Start-Sleep -s 30
# Runs the remote setup script in each VM, do retry if the connection fails.
foreach ($port in $result.Get_Item("ports")) {
  logStatus ("Running setup on " + $result.Get_Item("dns") + ":" + $port)
  $retry = 0;
  do {
    $success = $false;
    try 
    {
        Invoke-Command -ComputerName $result.Get_Item("dns") -Port $port -Credential $cred -FilePath $args[0] -ArgumentList ($result.Get_Item("ips")) -ErrorAction Stop
        $success = $true
    }
    catch [Exception]
    {
      If ($_.Exception -is [System.Management.Automation.Remoting.PSRemotingTransportException]) {
          if ($retry -ge 10) {
            $_.Exception.GetType().FullName
            logErr ("Connection to WinRM service running on $port failed after $retry retry, moving to next VM")
            break
          } else {
            logStatus2 ("Connection to WinRM service running on $port failed.. Retrying#" + $retry)
            Start-Sleep -s (20 + $retry*5) 
            $retry++;
          }
      } Else {
          throw $_.Exception
      }
    }
  } while (!$success);
}
