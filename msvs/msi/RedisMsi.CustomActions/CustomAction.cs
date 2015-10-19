using System;
using System.IO;
using Microsoft.Deployment.WindowsInstaller;
using System.ServiceProcess;

namespace RedisMsi.CustomActions
{
    /// <summary>
    /// Defines actions to take during the MSI install that don't
    /// come standard with WiX.
    /// </summary>
    public class CustomActions
    {
        /// <summary>
        /// Overwrites settings in the Redis config file using values from the installer.
        /// </summary>
        /// <param name="session">The install session context</param>
        /// <returns>Returns Success when the method completes. Exceptions will bubble up and 
        /// cause the installer to roll back.</returns>
        [CustomAction]
        public static ActionResult UpdateRedisConfig(Session session)
        {
            string configFilePath = session.CustomActionData["CONFIG_PATH"];
            if (File.Exists(configFilePath))
            {
                string originalContent = File.ReadAllText(configFilePath);

                string port = session.CustomActionData["PORT"];
                string updatedContent = originalContent.Replace("port 6379", "port " + port);

                string useMaxMemoryLimit = session.CustomActionData["USE_MEMORY_LIMITS"];
                if (useMaxMemoryLimit == "1")
                {
                    string strMaxMemoryMB = session.CustomActionData["MAXMEMORY"];
                    try
                    {
                        int intMaxMemory = Int32.Parse(strMaxMemoryMB);
                        if (intMaxMemory <= 0)
                        {
                            strMaxMemoryMB = "100";
                        }

                    }
                    catch
                    {
                        strMaxMemoryMB = "100";
                    }
                    updatedContent = updatedContent.Replace("# maxmemory <bytes>", "maxmemory " + strMaxMemoryMB + "mb");
                }

                File.WriteAllText(configFilePath, updatedContent);
            }
            else
            {
                throw new ApplicationException("UpdateRedisConfig: Config file not found. Could not update its settings.");
            }

            return ActionResult.Success;
        }

        /// <summary>
        /// Sets a WiX property to indicate whether the Windows Firewall service is stopped.
        /// If the firewall service is stopped, the install will not succeed if it attempts
        /// to add a firewall exception. Note that just setting the state of the 
        /// firewall to off does not pose a problem.
        /// </summary>
        /// <param name="session"></param>
        /// <returns></returns>
        [CustomAction]
        public static ActionResult CheckIfFirewallServiceRunning(Session session)
        {
            ServiceController sc = new ServiceController("MpsSvc"); // Windows Firewall service
            bool isStopped = sc.Status.Equals(ServiceControllerStatus.Stopped) || sc.Status.Equals(ServiceControllerStatus.StopPending);

            if (isStopped)
            {
                session["FIREWALL_SERVICE_STOPPED"] = "1";
            }

            return ActionResult.Success;
        }
    }
}
