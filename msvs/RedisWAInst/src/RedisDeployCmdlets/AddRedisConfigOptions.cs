using System;
using System.Diagnostics;
using System.IO;
using System.Management.Automation;
using System.Security.Permissions;

namespace RedisDeployCmdlets
{
    [Cmdlet(VerbsCommon.Add, "RedisConfigOptions")]
    public class AddRedisConfigOptions : PSCmdlet
    {
        [Parameter(Mandatory = true, HelpMessage = "Set the master port")]
        public string ListenPort { get; set; }

        [Parameter(Mandatory = true, HelpMessage = "Path to the redis configure file")]
        public string Path { get; set; }

        [Parameter(Mandatory = false, HelpMessage = "Set the master url")]
        public string MasterUrl { get; set; }

        [Parameter(Mandatory = false, HelpMessage = "Set the master port")]
        public string MasterPort { get; set; }

        [PermissionSet(SecurityAction.Demand, Name = "FullTrust")]
        protected override void ProcessRecord()
        {
            base.ProcessRecord();

            try
            {
                string opt = String.Format("port {0}\n", ListenPort);

                if (!String.IsNullOrEmpty(MasterUrl) && !String.IsNullOrEmpty(MasterPort))
                {
                    if (MasterUrl.ToUpper() != "LOCALHOST")
                    {
                        MasterUrl += ".cloudapp.net";
                    }

                    opt += String.Format("slaveof {0} {1}\n", MasterUrl, MasterPort);
                }
                File.AppendAllText(Path, String.Concat(opt));
            }
            catch (Exception ex)
            {
                WriteError(new ErrorRecord(ex, string.Empty, ErrorCategory.CloseError, null));
                throw;
            }

        }
    }
}
