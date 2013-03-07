Deploying Redis to Windows Azure
================================

1.	Prepare the ServiceConfiguration.cloud.cscfg, ServiceConfiguration.Local.cscfg and ServiceDefinition.csdef files for the Redis worker roles you want to deploy. The RedisWR\Samples folder has examples you can use as a starting point.
2.	Prepare a redis.conf file with the common Redis configuration settings that you wish to deploy. The same redis.conf is used as a base for the master and slave roles. The port numbers are automatically appended to each deployed redis.conf instance based on the cscfg file settings.
3.	From the bin folder, run the RedisInstWA command in a command Window running as an elevated Administrator. This application prepares the installation configuration and the files to deploy, and then uses Inst4WA to do the deployment.
4.	Inst4WA may install Azure SDK components it needs to run. If prompted to allow the installation please accept. 
5.	When deploying to Azure, if a publishsettings file is not provided then the default browser is used to download a publishsettings file for your account. You will need to sign in with your Azure account credentials. If you see a prompt in your browser to open or save the downloaded file, choose to save it. If you already have a publishsettings file with your credentials, copy it to the same folder as the ServiceDefinition.csdef file and it will be used. Alternatively you can provide a full path on the command line using a pass-through parameter.

RedisInstWA command line
------------------------

RedisInstWA -Source &lt;path to redis exe or URL to ZIP&gt; -Config &lt;path to cscfg file folder&gt; -RedisConf &lt;path to redis.conf file folder&gt; -Domain &lt;Azure domain&gt; -Subscription &lt;Azure subscription&gt; [-X64] [--Pass &lt;other parameters&gt;]

If deploying to the Azure emulator, then use -Emu in place of -Domain and -Subscription.
Note that the Source may be to local Redis exe files, or may reference a GitHub zip file. If using a zip file, the 32 bit binaries are used by default. To use the 64 bit binaries include the optional -X64 parameter.  
 
For example: -Source https://github.com/MSOpenTech/redis/archive/2.4.zip will download the zip file for the latest code for branch 2.4.

The --Pass (note the double '-') parameter is optional. Any parameters following this are passed on to Inst4WA. This is useful to override settings such as region or slot. See the Pass-through parameters section.

### Deployment notes
If deploying to the Azure emulator, you will need to manually remove the deployment after you have finished testing. You should also remove a previous deployment before trying to deploy a second time.  
After downloading files, you may have to unblock the DLLs to enable them to be used. Please follow instructions at [http://msdn.microsoft.com/en-us/library/ee890038(VS.100).aspx](http://msdn.microsoft.com/en-us/library/ee890038(VS.100).aspx)

### Pass-through parameters
* StorageAccountName - default DomainName + 'stg'
* DeploymentName - default DomainName + 'deploy'
* Region - default 'West US'
* DeploymentOption - default 'Production'
* PublishSettingsFilePath - default is any file in –Config folder with publishsettings extension.

Configuring the deployment
--------------------------
ServiceDefinition.csdef defines the roles to be deployed.  

- Port numbers to use are defined here for each role.  
- You also set the VMSize for each role.  
- The sample defines a Startup task that runs startup.cmd. This opens the firewall ports for redis-server.exe. You should follow the sample if you create your own csdef file.

The cscfg file contains settings used by the Azure roles.

- The osFamily attribute in the samples is set to "1" for Win2K8R2. You can change this to use a different OS.

To enable RDP to the Azure roles, you need to define the certificates that you will use for RDP to Azure in the cscfg file. To define the certificate, add an XML element under each Role that specifies the thumbprint for the certificate you are using.  
For example:   

>     <Role name="RedisMaster">  
>       …  
>       <Certificates>   
>        <Certificate name="Microsoft.WindowsAzure.Plugins.RemoteAccess.PasswordEncryption"   
>                     thumbprint="2CF79A6E4B88031BB3C70B3A6A26D86F2F687111"   
>                     thumbprintAlgorithm="sha1" />  
>       </Certificates>  
>     </Role>   
>
   

Implementation detail
---------------------
RedisInstWA creates a folder in the current working directory called RedisInstWork. All the files necessary for deployment are copied here.

It makes a copy of the redis exe files to deploy in the release sub folder.  It copies the redis.conf file here. It also creates an XmlConfig.xml file that is used as input by the Inst4WA application.

Once this is done, it launches Inst4WA. Inst4WA uses the XmlConfig.xml file as input to pass parameters to Powershell cmdlets for doing the Azure deployment. You can view this xml file to view the parameters passed to the cmdlets. This may be useful in determining if you want to use pass-through parameters.

The subfolder DeployRedis is used by the installer to create the package to be sent to Azure.

Troubleshooting
---------------
The installer may fail for several reasons. Each step in the installation produces text output in the command window to help you understand where it failed.

If there was a failure coming from Azure, the text message may not contain the information that you need to understand why it failed. 

In this case you should inspect the operation logs using the Azure portal. In the portal page select the settings tab on the left. From there select the operational logs on the top. You may need to scroll to find the most recent logs. Click on the relevant log entry to view the failure message.

Building the sources
--------------------
To build the sources load the RedisWaInst solution in Visual Studio 2010, select the Release configuration and Build.

Binaries will be placed in a folder called RedisInstBin. The binaries in the bin folder will not be replaced. If you need to modify the bin folder executables, copy the updated binaries from src\RedisInstBin.

Acknowledgements
----------------
The Inst4WA binaries were obtained from [https://github.com/MSOpenTech/inst4wa](https://github.com/MSOpenTech/inst4wa).

