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

using Shell32;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
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
        static string passThough;
        static bool IsX64;

        // Values from environment or configuration
        static string WorkPath;
        static string RedisInstExePath;
        static string PubSettingFilePath;
        static List<RedisInstance> Instances;

        const string WorkDir = "RedisInstWork";
        const string BinariesSubDir = "release";

        /// <summary>
        /// Prepare and initiate install of Redis to Azure
        /// </summary>
        /// <param name="args"></param>
        static void Main(string[] args)
        {

            if (!IsElevated)
            {
                Console.WriteLine("RedisInstWA must be run as an Administrator");
                Environment.Exit(1);
            }
            if (args == null || args.Count() == 0)
            {
                Console.WriteLine("No parameters were provided\r\n");
                Usage();
                Environment.Exit(1);
            }
            else
            {
                if (!ValidateArgs(args))
                {
                    Console.WriteLine("Usage:");
                    Usage();
                    Environment.Exit(1);
                }
                // create work directory for copying files
                string curpath = Directory.GetCurrentDirectory();
                WorkPath = Path.Combine(curpath, WorkDir);
                if (!FolderHelper.MakeFolder(WorkPath))
                {
                    Console.WriteLine("Failed to make folder for staging the installation");
                    Environment.Exit(2);
                }



                RedisInstExePath = Assembly.GetExecutingAssembly().Location;
                RedisInstExePath = Path.GetDirectoryName(RedisInstExePath);
                // Download and extract exe from zip file or copy local exe
                if (!PrepareBinaries(WorkPath))
                {
                    Console.WriteLine("Failed to copy redis executable files to staging directory");
                    Environment.Exit(2);
                }

                // Parse the ServiceDefinition to extract roles and ports
                if (!ParseConfiguration())
                {
                    Console.WriteLine("Failed to parse the ServiceDefinition file");
                    Environment.Exit(3);
                }

                // Prepare instances and xml file for Inst4WA
                if (!PrepareInstaller())
                {
                    Console.WriteLine("Failed to create the XML Configuration file for the installer");
                    Environment.Exit(4);
                }

                // run Inst4WA to do install
                if (!RunInstaller())
                {
                    Console.WriteLine("Failed to execute Inst4WA process to install Redis");
                    Environment.Exit(5);
                }
            }
            Environment.Exit(0);
        }

        /// <summary>
        /// Validate input parameters
        /// </summary>
        /// <param name="args"></param>
        /// <returns></returns>
        private static bool ValidateArgs(string[] args)
        {
            bool isSource = false;
            bool isConfig = false;
            bool isRedisConf = false;
            bool isEmuOrAzure = false;
            passThough = "";
            IsX64 = false;

            for (int i = 0; i < args.Length; i++)
            {
                if (args[i].StartsWith("-So", StringComparison.InvariantCultureIgnoreCase))
                {
                    if (i == args.Length - 1)
                    {
                        Console.WriteLine("Missing argument after: " + args[i]);
                        return false;
                    }
                    i++;
                    if (args[i].StartsWith("http", StringComparison.InvariantCultureIgnoreCase))
                    {
                        if (Uri.IsWellFormedUriString(args[i], UriKind.Absolute) == false)
                        {
                            Console.WriteLine("Source URL format is invalid: " + args[i]);
                            return false;
                        }
                        GitUrl = args[i];
                        IsLocal = false;
                    }
                    else if (!Directory.Exists(args[i]))
                    {
                        Console.WriteLine("Source path does not exist: " + args[i]);
                        return false;
                    }
                    else
                    {
                        string sourcePath = Path.GetFullPath(args[i]);
                        string[] files = Directory.GetFiles(sourcePath, "redis-server.exe");
                        if (files == null || files.Length == 0)
                        {
                            Console.WriteLine("Source path does not contain redis-server.exe: " + args[i]);
                            return false;
                        }
                        IsLocal = true;
                        LocalExePath = sourcePath;
                    }
                    isSource = true;
                }
                else if (args[i].StartsWith("-Co", StringComparison.InvariantCultureIgnoreCase))
                {
                    if (i == args.Length - 1)
                    {
                        Console.WriteLine("Missing argument after: " + args[i]);
                        return false;
                    }
                    i++;
                    if (!Directory.Exists(args[i]))
                    {
                        Console.WriteLine("Configuration path does not exist: " + args[i]);
                        return false;
                    }
                    else
                    {
                        string configPath = Path.GetFullPath(args[i]);
                        string[] files = Directory.GetFiles(configPath, "ServiceDefinition.csdef");
                        if (files == null || files.Length == 0)
                        {
                            Console.WriteLine("Configuration path does not contain ServiceDefinition.csdef: " + args[i]);
                            return false;
                        }
                        files = Directory.GetFiles(configPath, "ServiceConfiguration.Local.cscfg");
                        if (files == null || files.Length == 0)
                        {
                            Console.WriteLine("Configuration path does not contain ServiceConfiguration.Local.cscfg: " + args[i]);
                            return false;
                        }
                        files = Directory.GetFiles(configPath, "ServiceConfiguration.Cloud.cscfg");
                        if (files == null || files.Length == 0)
                        {
                            Console.WriteLine("Configuration path does not contain ServiceConfiguration.Cloud.cscfg: " + args[i]);
                            return false;
                        }
                        files = Directory.GetFiles(configPath, "*.publishsettings");
                        if (files != null && files.Length > 0)
                        {
                            // found at least one publishsettings file. Choose first one.
                            PubSettingFilePath = files[0];
                        }
                        else
                        {
                            PubSettingFilePath = null;
                        }
                        ConfigFolderPath = configPath;
                        isConfig = true;
                    }
                }
                else if (args[i].StartsWith("-Re", StringComparison.InvariantCultureIgnoreCase))
                {
                    if (i == args.Length - 1)
                    {
                        Console.WriteLine("Missing argument after: " + args[i]);
                        return false;
                    }
                    i++;
                    if (!Directory.Exists(args[i]))
                    {
                        Console.WriteLine("RedisConf path does not exist: " + args[i]);
                        return false;
                    }
                    else
                    {
                        string redisconfPath = Path.GetFullPath(args[i]);
                        string[] files = Directory.GetFiles(redisconfPath, "redis.conf");
                        if (files == null || files.Length == 0)
                        {
                            Console.WriteLine("RedisConf path does not contain redis.conf: " + args[i]);
                            return false;
                        }
                        RedisConfPath = redisconfPath;
                        isRedisConf = true;
                    }
                }
                else if (args[i].StartsWith("-Em", StringComparison.InvariantCultureIgnoreCase))
                {
                    if (isEmuOrAzure && !IsEmul)
                    {
                        Console.WriteLine("Both -Emul and -Domain or -Subscription may not be specified ");
                        return false;
                    }
                    IsEmul = true;
                    isEmuOrAzure = true;
                }
                else if (args[i].StartsWith("-Do", StringComparison.InvariantCultureIgnoreCase))
                {
                    if (i == args.Length - 1)
                    {
                        Console.WriteLine("Missing argument after: " + args[i]);
                        return false;
                    }
                    i++;
                    if (isEmuOrAzure && IsEmul)
                    {
                        Console.WriteLine("Both -Emul and -Domain may not be specified ");
                        return false;
                    }
                    isEmuOrAzure = true;
                    IsEmul = false;
                    Domain = args[i];
                }
                else if (args[i].StartsWith("-Su", StringComparison.InvariantCultureIgnoreCase))
                {
                    if (i == args.Length - 1)
                    {
                        Console.WriteLine("Missing argument after: " + args[i]);
                        return false;
                    }
                    i++;
                    if (isEmuOrAzure && IsEmul)
                    {
                        Console.WriteLine("Both -Emul and -Subscription may not be specified ");
                        return false;
                    }
                    isEmuOrAzure = true;
                    IsEmul = false;
                    Subscription = args[i];
                }
                else if (args[i].StartsWith("--Pa", StringComparison.InvariantCultureIgnoreCase))
                {
                    // Following parameters are passed on to Inst4WA
                    passThough = " ";
                    i++;
                    while (i < args.Length)
                    {
                        // put remaining args into a string with space spearator
                        // If arg doesn't start with -, enclose with quotes
                        if (args[i].StartsWith("-"))
                            passThough += args[i] + " ";
                        else
                            passThough += @"""" + args[i] + @""" ";
                        i++;
                    }
                    break;
                }
                else if (args[i].StartsWith("-X64", StringComparison.InvariantCultureIgnoreCase))
                {
                    IsX64 = true;
                }
                else
                {
                    Console.WriteLine("Unknown option specified " + args[i]);
                    return false;
                }
            }
            if (!isSource)
            {
                Console.WriteLine("Missing -Source argument");
                return false;
            }
            if (!isConfig)
            {
                Console.WriteLine("Missing -Configuration argument");
                return false;
            }
            if (!isRedisConf)
            {
                Console.WriteLine("Missing -RedisConf argument");
                return false;
            }
            if (!isEmuOrAzure)
            {
                Console.WriteLine("Missing -Emul or -Domain and -Subscription argument");
                return false;
            }
            if (!IsEmul && String.IsNullOrWhiteSpace(Domain))
            {
                Console.WriteLine("Missing -Domain argument");
                return false;
            }
            if (!IsEmul && String.IsNullOrWhiteSpace(Subscription))
            {
                Console.WriteLine("Missing -Subscription argument");
                return false;
            }

            return true;
        }

        /// <summary>
        /// Place installable files in folders
        /// </summary>
        /// <param name="workPath"></param>
        /// <returns></returns>
        private static bool PrepareBinaries(string workPath)
        {
            string WorkBinariesPath  = Path.Combine(workPath, BinariesSubDir);
            bool isCopied = false;

            if (!IsLocal)
            {
                ExtractZip exz = new ExtractZip();
                int rc = exz.DownloadAndExtract(GitUrl, workPath, BinariesSubDir, IsX64);
                isCopied = rc == 0;
            }
            else
            {
                if (!FolderHelper.MakeFolder(workPath))
                    return false;

                if (LocalExePath != WorkBinariesPath)
                {
                    if (!FolderHelper.MakeFolderReset(WorkBinariesPath))
                        return false;

                    Shell sh = new Shell();
                    Folder outdir = sh.NameSpace(WorkBinariesPath);
                    IEnumerable<string> subfiles = Directory.EnumerateFiles(LocalExePath, "*.exe");
                    foreach (string file in subfiles)
                    {
                        string filepath = Path.Combine(LocalExePath, file);
                        outdir.CopyHere(filepath, 0);  // 4 for no progress dialog, 16 for Yes to all
                    }
                    subfiles = Directory.EnumerateFiles(LocalExePath, "*.pdb");
                    foreach (string file in subfiles)
                    {
                        string filepath = Path.Combine(LocalExePath, file);
                        outdir.CopyHere(filepath, 0);  // 4 for no progress dialog, 16 for Yes to all
                    }
                }

                // return true if redis-server exists
                isCopied = File.Exists(Path.Combine(WorkBinariesPath, @"redis-server.exe"));
            }

            if (isCopied)
            {
                // copy redis.conf file to workPath
                string redisconf = File.ReadAllText(Path.Combine(RedisConfPath, @"redis.conf"));
                File.WriteAllText(Path.Combine(workPath, @"redis.conf"), redisconf);
            }
            return isCopied;
        }

        /// <summary>
        /// Parse csdef file to extract installation properties
        /// </summary>
        /// <returns></returns>
        private static bool ParseConfiguration()
        {
            string pathCsdef = Path.Combine(ConfigFolderPath, "ServiceDefinition.csdef");

            Instances = ParseConfig.Parse(pathCsdef);
            return Instances.Count > 0;
        }

        /// <summary>
        /// Prepare XML file used as input to Inst4WA
        /// </summary>
        /// <returns></returns>
        private static bool PrepareInstaller()
        {
            return InstWaXml.CreateInstXml(IsEmul,
                                        ConfigFolderPath,
                                        WorkPath,
                                        WorkPath,
                                        RedisInstExePath,
                                        BinariesSubDir,
                                        Domain,
                                        Subscription,
                                        PubSettingFilePath,
                                        Instances);
        }

        private static bool RunInstaller()
        {
            string args = "-XmlConfigPath " + @"""" + Path.Combine(WorkPath, "XmlConfig.xml") + @"""";
            if (!IsEmul)
            {
                // append DomainName and Subscription, enclosing values in quotes
                args = args + " -DomainName " + @"""" + Domain + @"""" + " -Subscription " + @"""" + Subscription + @"""";
            }
            // append passthrough args
            args = args + passThough;

            // create process info to run inst4wa
            Process p = new Process();

            p.StartInfo.FileName = Path.Combine(RedisInstExePath, @"Inst4WA\Inst4WA.exe");
            p.StartInfo.Arguments = args;
            p.StartInfo.UseShellExecute = false;
            p.StartInfo.WorkingDirectory = Directory.GetCurrentDirectory();

            Console.WriteLine("Launching " + p.StartInfo.FileName + " " + p.StartInfo.Arguments);
            p.Start();

            p.WaitForExit();
            p.Close();

            return true;
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
                         "            [--Pass <other parameters>]\r\n" +
                         "Where:\r\n" +
                         "-Source <Github URL to ZIP>: URL to download redis. For example:\r\n" + 
                         "        'https://github.com/MSOpenTech/redis/archive/2.4.zip'\r\n" +
                         "-Source <path to redis-server.exe>: local path to folder with redis exe files\r\n" +
                         "-X64: Use this to deploy 64bit binaries. If omitted then 32bit binaries are used\r\n" +
                         "-Config <path to install configuration>: local path to folder with .csdef file\r\n" +
                         "-RedisConf <path to redis.conf>: local path to folder with redis.conf file\r\n" +
                         "-Emu: Use this if deploying to emulator\r\n" +
                         "-Domain <Azure domain>: Name to be used to create the hosted service in Azure\r\n" +
                         "-Subscription <Azure subscription>: Windows Azure subscription name (not ID)\r\n" +
                         "--Pass <other parameters>: Optional. Parameters after --Pass are for Inst4WA\r\n" +
                         "       This can be used to override settings such as Region\r\n" +
                         "\r\n" +
                         "Notes:\r\n" +
                         "Only the first 2 characters of each option are needed (-So is same as -Source)\r\n\r\n" +
                         "The installer must be run with elevated Administrator privileges\r\n\r\n" +
                         "Specify one redis.conf for all instances.\r\n" +
                         "  For each instance the port number is appended.\r\n" + 
                         "  For each slave, the master endpoint is appended\r\n" +
                         "\r\n";
            Console.WriteLine(msg);
        }
    }
}
