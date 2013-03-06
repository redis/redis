#region Copyright Notice
/*
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
*/
#endregion


namespace RedisInstWA
{
    using System;
    using System.Collections.Generic;
    using System.IO;
    using System.Linq;
    using System.Text;

    /// <summary>
    /// Create XML file to control Inst4WA utility
    /// </summary>
    public class InstWaXml
    {
        //Fragments of the XML file to control Inst4WA are defined here
        // The parameters are used to customize the XML file

        private const string instXmlSkeleton = 
@"<DeploymentModel  xmlns:xsi=""http://www.w3.org/2001/XMLSchema-instance"" xmlns:xsd=""http://www.w3.org/2001/XMLSchema"">
  <Parameters>
{0}
  </Parameters>
  <Steps>
{1}
  </Steps>
</DeploymentModel>
";

        // emul value is true or false
        private const string instXmlEmul = 
@"    <!-- Emulator parameter is required. Yes or No -->
    <Parameter Name=""Emulator"" Value=""{0}""></Parameter>
";

        // Set up folder path variables
        // {0} is work folder path 
        // {1} is Redis Azure installer binaries folder path
        // {2} is subfolder for redis exe files (binaries)
        // {3} is path to csdef and cscfg files
        // {4} is path to redis.conf input file
        private const string instXmlRoot =
@"    <!-- replaceable values for root dir-->
    <Parameter Name=""RootDir"" Value=""{0}""></Parameter>
    <Parameter Name=""BinDir"" Value=""{1}""></Parameter>
    <Parameter Name=""RedisServerDir"" Value=""{0}\{2}""></Parameter>
    <Parameter Name=""RedisPkgDir"" Value=""{1}\RedisPkgBin""></Parameter>
    <Parameter Name=""AzureConfigDir"" Value=""{3}""></Parameter>
    <Parameter Name=""RedisConfFile"" Value=""{4}\redis.conf""></Parameter>
    <Parameter Name=""AppFolder"" ValuePrefixRef=""RootDir"" ValueSuffix=""\DeployRedis""></Parameter>
    <Parameter Name=""AppName"" Value=""RedisAzure""></Parameter>
";

        // Set up publish setting file path
        // {0} is file path path 
        private const string instXmlPubSettingsFile =
@"    <!-- value for publish settings -->
    <Parameter Name=""PublishSettingsFilePath"" Value=""{0}""></Parameter>
";

        // paths for SDK components
        private const string instXmlSDK =
@"    <!-- assemblies and SDKs -->
    <Parameter Name=""AzureDeploymentCmdletsAssemblyPath""
               Value=""%%Program Files%%\Microsoft SDKs\Windows Azure\PowerShell\Azure\Microsoft.WindowsAzure.Management.CloudService.dll"" Required=""yes""></Parameter>
    <Parameter Name=""AzureMgmtCmdletsAssemblyPath""
           Value=""%%Program Files%%\Microsoft SDKs\Windows Azure\PowerShell\Azure\Microsoft.WindowsAzure.Management.dll"" Required=""yes""></Parameter>
    <Parameter Name=""RedisDeployCmdletsAssemblyPath""
               ValuePrefixRef=""BinDir"" ValueSuffix=""\RedisDeployCmdlets.dll"" Required=""yes""></Parameter>
    <Parameter Name=""AzureNodeJsSdkPath"" Value=""%%Program Files%%\Microsoft SDKs\Windows Azure\PowerShell\Azure""></Parameter>
";

        // Azure settings
        // {0} is Domain 
        // {1} is subscription
        private const string instXmlAzure =
@"    <Parameter Name=""DomainName"" Value=""{0}"" Required=""yes""></Parameter>
    <Parameter Name=""Subscription"" Value=""{1}"" Required=""yes""></Parameter>
    <Parameter Name=""StorageAccountName"" ValuePrefixRef=""DomainName"" ValueSuffix=""stg"" Required=""yes""></Parameter>
    <Parameter Name=""DeploymentName"" ValuePrefixRef=""DomainName"" ValueSuffix=""deploy"" Required=""yes""></Parameter>
    <Parameter Name=""Region"" Value=""West US""></Parameter>
    <Parameter Name=""DeploymentOption"" Value=""Production""></Parameter>
";

        // Config files
        // {0} is local or cloud
        private const string instXmlAzureCfg =
@"    <Parameter Name=""CSCFGFile"" ValuePrefixRef=""AzureConfigDir"" ValueSuffix=""\ServiceConfiguration.{0}.cscfg""></Parameter>
    <Parameter Name=""CSDEFFile"" ValuePrefixRef=""AzureConfigDir"" ValueSuffix=""\ServiceDefinition.csdef""></Parameter>
";

        // Roles
        // {0} is role index
        // {1} is role name
        // {2} is role internal listening port
        private const string instXmlRole =
@"    <Parameter Name=""RoleName{0}"" Value=""{1}""></Parameter>
    <Parameter Name=""RoleDest{0}"" ValuePrefixRef=""AppFolder"" ValueSuffix=""\RedisAzure\{1}""></Parameter>
    <Parameter Name=""RolePort{0}"" Value=""{2}""></Parameter>
    <Parameter Name=""RoleRedisConf{0}"" ValuePrefixRef=""RoleDest{0}"" ValueSuffix=""\redis.conf""></Parameter>
";

        // Master role
        // {0} is master port (external)
        // {1} is master host name or IP
        private const string instXmlMasterRole =
@"    <Parameter Name=""MasterPort"" Value=""{0}""></Parameter>
    <Parameter Name=""MasterHost"" Value=""{1}""></Parameter>
";

        // common steps to add assemblies and create folder
        private const string instXmlStepPrep =
@"    <Step Type=""Cmdlet"" Command=""Install-AzureSdkForNodeJs"" Message=""Installing Windows Azure PowerShell for Node.JS"">
      <CommandParam Name=""AzureNodeSdkLoc"" ParameterName=""AzureNodeJsSdkPath"" />
    </Step>
    <!--Load Azure SDK Dll's-->
    <Step Type=""Cmdlet"" Command=""Add-LoadAssembly"" Message=""Loading Windows Azure PowerShell for Node.JS"">
      <CommandParam Name=""CmdletsAssemblyPath"" ParameterName=""AzureDeploymentCmdletsAssemblyPath"" />
    </Step>
    <Step Type=""Cmdlet"" Command=""Add-LoadAssembly"" Message=""Loading Windows Azure PowerShell for Node.JS"">
      <CommandParam Name=""CmdletsAssemblyPath"" ParameterName=""AzureMgmtCmdletsAssemblyPath"" />
    </Step>
    <Step Type=""Cmdlet"" Command=""Add-LoadAssembly"" Message=""Loading RedisDeployCmdlets"">
      <CommandParam Name=""CmdletsAssemblyPath"" ParameterName=""RedisDeployCmdletsAssemblyPath"" />
    </Step>
    <!--Create Empty App-->
    <Step Type=""Cmdlet"" Command=""New-ApplicationDirectory"" Message=""Creating application directory"">
      <CommandParam Name=""AppFolder"" ParameterName=""AppFolder"" />
    </Step>
    <Step Message=""Changing to application directory"" Command="""" Type=""ChangeWorkingDir"">
      <CommandParam Name=""WorkingDir"" ParameterName=""AppFolder""/>
    </Step>
    <Step Message=""Creating new Azure service"" Command=""New-AzureServiceProject"" Type=""Cmdlet"">
      <CommandParam Name=""ServiceName"" ParameterName=""AppName""/>
    </Step>
";

        // steps to publish settings if not emul
        private const string instXmlStepPrepProd =
@"    <Step Type=""Cmdlet"" Command=""Import-AzurePublishSettingsFile"" Message=""Importing Publish settings"">
      <CommandParam Name=""PublishSettingsFile"" ParameterName=""PublishSettingsFilePath"" />
    </Step>
";

        // Master role install
        // {0} is index for role - should be 0
        private const string instXmlMasterRoleStep =
@"    <Step Type=""Cmdlet"" Command=""DeployCmdlets4WA\Add-AzureWorkerRole"" Message=""Creating Redis worker role."">
      <CommandParam Name=""RoleBinariesFolder"" ParameterName=""RedisPkgDir"" />
      <CommandParam Name=""CSCFGFile"" ParameterName=""CSCFGFile"" />
      <CommandParam Name=""CSDEFFile"" ParameterName=""CSDEFFile"" />
      <CommandParam Name=""RoleName"" ParameterName=""RoleName{0}"" />
    </Step>
    <Step Type=""Cmdlet"" Command=""Copy-Item"" Message=""Copying Redis exe to role."">
      <CommandParam Name=""Path"" ParameterName=""RedisServerDir"" />
      <CommandParam Name=""Destination"" ParameterName=""RoleDest{0}"" />
      <CommandParam Name=""Recurse"" ParameterName="""" />
    </Step>
    <Step Type=""Cmdlet"" Command=""Copy-Item"" Message=""Copying Redis.conf to role."">
      <CommandParam Name=""Path"" ParameterName=""RedisConfFile"" />
      <CommandParam Name=""Destination"" ParameterName=""RoleDest{0}"" />
    </Step>
    <Step Type=""Cmdlet"" Command=""Add-RedisConfigOptions"" Message=""Added config options to redis master .conf file."">
      <CommandParam Name=""ListenPort"" ParameterName=""RolePort{0}"" />
      <CommandParam Name=""Path"" ParameterName=""RoleRedisConf{0}"" />
    </Step>
";

        // Slave role install
        // {0} is index for role
        private const string instXmlSlaveRoleStep =
@"    <Step Type=""Cmdlet"" Command=""DeployCmdlets4WA\Add-AzureWorkerRole"" Message=""Creating Redis worker role."">
      <CommandParam Name=""RoleBinariesFolder"" ParameterName=""RedisPkgDir"" />
      <CommandParam Name=""CSCFGFile"" ParameterName=""CSCFGFile"" />
      <CommandParam Name=""CSDEFFile"" ParameterName=""CSDEFFile"" />
      <CommandParam Name=""RoleName"" ParameterName=""RoleName{0}"" />
    </Step>
    <Step Type=""Cmdlet"" Command=""Copy-Item"" Message=""Copying Redis exe to role."">
      <CommandParam Name=""Path"" ParameterName=""RedisServerDir"" />
      <CommandParam Name=""Destination"" ParameterName=""RoleDest{0}"" />
      <CommandParam Name=""Recurse"" ParameterName="""" />
    </Step>
    <Step Type=""Cmdlet"" Command=""Copy-Item"" Message=""Copying Redis.con to role."">
      <CommandParam Name=""Path"" ParameterName=""RedisConfFile"" />
      <CommandParam Name=""Destination"" ParameterName=""RoleDest{0}"" />
    </Step>
    <Step Type=""Cmdlet"" Command=""Add-RedisConfigOptions"" Message=""Added config options to redis master .conf file."">
      <CommandParam Name=""ListenPort"" ParameterName=""RolePort{0}"" />
      <CommandParam Name=""Path"" ParameterName=""RoleRedisConf{0}"" />
      <CommandParam Name=""MasterUrl"" ParameterName=""MasterHost"" />
      <CommandParam Name=""MasterPort"" ParameterName=""MasterPort"" />
    </Step>
";

        // Launch for emul
        private const string instXmlLaunchEmul =
@"    <Step Type=""Cmdlet"" Command=""Start-AzureEmulator -launch"" Message=""Deploying Redis inside Emulator"">
    </Step>
";

        // Launch for Azure
        private const string instXmlDeployAzure =
@"    <Step Type=""Cmdlet"" Command=""Set-AzureStorageAccountEx"" Message=""Configuring azure storage account"">
      <CommandParam Name=""PublishSettingsFile"" ParameterName=""PublishSettingsFilePath"" />
      <CommandParam Name=""StorageAccount"" ParameterName=""StorageAccountName"" />
      <CommandParam Name=""Location"" ParameterName=""Region"" />
      <CommandParam Name=""Subscription"" ParameterName=""Subscription"" />
    </Step>
    <Step Type=""Cmdlet"" Command=""Publish-AzureServiceProject -launch"" Message=""Deploying the app to Azure"">
      <CommandParam Name=""ServiceName"" ParameterName=""DeploymentName"" />
      <CommandParam Name=""StorageAccountName"" ParameterName=""StorageAccountName"" />
      <CommandParam Name=""Slot"" ParameterName=""DeploymentOption"" />
      <CommandParam Name=""Location"" ParameterName=""Region"" />
      <CommandParam Name=""Subscription"" ParameterName=""Subscription"" />
    </Step>
    <Step Type=""Cmdlet"" Command=""Ping-ServiceEndpoints"" Message=""Verifying the Endpoints."">
      <CommandParam Name=""PublishSettingsFile"" ParameterName=""PublishSettingsFilePath"" />
      <CommandParam Name=""ServiceName"" ParameterName=""DeploymentName"" />
      <CommandParam Name=""Subscription"" ParameterName=""Subscription"" />
    </Step>
";

        /// <summary>
        /// Create the XmlConfig.xml file
        /// </summary>
        /// <param name="isEmul"></param>
        /// <param name="configPath"></param>
        /// <param name="redisConfPath"></param>
        /// <param name="workPath"></param>
        /// <param name="redisInstBinPath"></param>
        /// <param name="redisBinSubFolder"></param>
        /// <param name="domain"></param>
        /// <param name="subscription"></param>
        /// <param name="instances"></param>
        /// <returns></returns>
        public static bool CreateInstXml(bool isEmul,
                                    string configPath,
                                    string redisConfPath,
                                    string workPath,
                                    string redisInstBinPath,
                                    string redisBinSubFolder,
                                    string domain,
                                    string subscription,
                                    string pubSettingsFilePath,
                                    List<RedisInstance> instances)
        {
            RedisInstance masterInst = instances[0];
            int masterIndex = 0;

            StringBuilder sbParams = new StringBuilder();
            sbParams.AppendFormat(instXmlEmul, isEmul ? "true" : "false");
            sbParams.AppendFormat(instXmlRoot, workPath, redisInstBinPath, redisBinSubFolder, configPath, redisConfPath);
            sbParams.AppendFormat(instXmlPubSettingsFile, pubSettingsFilePath == null ? "" : pubSettingsFilePath);
            sbParams.Append(instXmlSDK);
            if (!isEmul)
            {
                sbParams.AppendFormat(instXmlAzure, domain, subscription);
            }
            sbParams.AppendFormat(instXmlAzureCfg, isEmul ? "local" : "cloud");

            int roleIndex = 0;
            foreach (RedisInstance inst in instances)
            {
                if (inst.IsMaster)
                {
                    masterInst = inst;
                    masterIndex = roleIndex;
                }
                sbParams.AppendFormat(instXmlRole, roleIndex, inst.RoleName, inst.LocalPort);
                roleIndex++;
            }
            sbParams.AppendFormat(instXmlMasterRole, masterInst.MasterPort, isEmul ? "localhost" : domain);

            StringBuilder sbSteps = new StringBuilder();
            sbSteps.Append(instXmlStepPrep);
            if (!isEmul)
            {
                sbSteps.Append(instXmlStepPrepProd);
            }

            sbSteps.AppendFormat(instXmlMasterRoleStep, masterIndex);
            roleIndex = 0;
            foreach (RedisInstance inst in instances)
            {
                if (!inst.IsMaster)
                {
                    sbSteps.AppendFormat(instXmlSlaveRoleStep, roleIndex);
                }
                roleIndex++;
            }

            if (isEmul)
            {
                sbSteps.Append(instXmlLaunchEmul);
            }
            else
            {
                sbSteps.Append(instXmlDeployAzure);
            }

            string xmlconfig = String.Format(instXmlSkeleton, sbParams.ToString(), sbSteps.ToString());

            try
            {
                File.WriteAllText(Path.Combine(workPath, "XmlConfig.xml"), xmlconfig, Encoding.UTF8);
            }
            catch (Exception ex)
            {
                if (ex.InnerException != null)
                    Console.WriteLine(ex.InnerException.Message);
                else
                    Console.WriteLine(ex.Message);
                return false;
            }

            Console.WriteLine("XmlConfig file for Inst4WA written as");
            Console.WriteLine(Path.Combine(workPath, "XmlConfig.xml"));
            return true;
        }
    }
}
