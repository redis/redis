using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Principal;
using System.Text;

namespace RedisInstWA
{
    class Program
    {
        // values from input args
        static string GitUrl;
        static string LocalExePath;
        static bool IsLocal;
        static string ConfigFolderPath;
        static string RedisConfPath;
        static bool IsEmul;
        static string Domain;
        static string Subscription;

        /// <summary>
        /// Prepare and initiate install of Redis to Azure
        /// </summary>
        /// <param name="args"></param>
        static void Main(string[] args)
        {

            if (!IsElevated || (args == null) || (args.Count() == 0))
            {
                Usage();
            }
            else
            {
                if (!ValidateArgs(args))
                {
                    Usage();
                    Environment.Exit(1);
                }
                // Download and extract exe from zip file or copy local exe
                PrepareBinaries();
                // Prepare instances and xml file for Inst4WA
                PrepareInstaller();
                // run Inst4WA to do install
                RunInstaller();
            }
        }

        private static bool ValidateArgs(string[] args)
        {
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i].StartsWith("-So", StringComparison.InvariantCultureIgnoreCase))
                {
                    i++;
                    if (args[i].StartsWith("-", StringComparison.InvariantCultureIgnoreCase))
                    {
                        Console.WriteLine("Source url or path not valid " + args[i]);
                        return false;
                    }
                    if (args[i].StartsWith("http", StringComparison.InvariantCultureIgnoreCase))
                    {
                    }
                }
                else if (args[i].StartsWith("-Co", StringComparison.InvariantCultureIgnoreCase))
                {
                    i++;
                }
                else if (args[i].StartsWith("-Re", StringComparison.InvariantCultureIgnoreCase))
                {
                    i++;
                }
                else if (args[i].StartsWith("-Em", StringComparison.InvariantCultureIgnoreCase))
                {

                }
                else if (args[i].StartsWith("-Do", StringComparison.InvariantCultureIgnoreCase))
                {
                    i++;
                }
                else if (args[i].StartsWith("-Su", StringComparison.InvariantCultureIgnoreCase))
                {
                    i++;
                }
                else
                {
                    Console.WriteLine("Unknown option specified " + args[i]);
                    return false;
                }
            }
        }

        private static void PrepareBinaries()
        {
        }

        private static void PrepareInstaller()
        {
        }

        private static void RunInstaller()
        {
        }

        private static bool IsElevated
        {
            get
            {
                return new WindowsPrincipal
                    (WindowsIdentity.GetCurrent()).IsInRole
                    (WindowsBuiltInRole.Administrator);
            }
        }
        
        private static void Usage()
        {
            string msg = "RedisInstWA installs Redis to Windows Azure.\r\n" +
                         "RedisInstWA -Source <Github URL to ZIP> | <path to redis-server.exe>\r\n" +
                         "            -Config <path to install configuration>\r\n" +
                         "            -RedisConf <path to redis.conf>\r\n" +
                         "            -Emu | -Domain <Azure domain> -Subscription <Azure subscription>\r\n" +
                         "Where:\r\n" +
                         "<Github URL to ZIP>: URL to download such as 'https://github.com/MSOpenTech/redis/archive/2.4.zip'\r\n" +
                         "<path to redis-server.exe>: local path to redis executables to deploy\r\n" +
                         "<path to install configuration>: local path to .csdef file specifying Master and Slave roles\r\n" +
                         "-Emu: Use this if deploying to emulator\r\n" +
                         "<Azure domain>: Unique name to be used to create the service to be deployed\r\n" +
                         "<Azure subscription>: Windows Azure subscription name\r\n" +
                         "\r\n" +
                         "Notes:\r\n" +
                         "Only the first 2 characters of each option are needed (-So is same as -Source)\r\n" +
                         "The installer must be run with elevated Administrator privileges\r\n" +
                         "Specify one redis.conf for all instances.\r\n" +
                         "  For each instance the port number is appended. For each slave, the master endpoint is appended\r\n" +
                         "\r\n";
            Console.WriteLine(msg);
        }
    }
}
