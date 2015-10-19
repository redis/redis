using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.IO;
using System.IO.Compression;
using System.Xml;
using System.Reflection;
using System.Diagnostics;

namespace ReleasePackagingTool
{
    class Program
    {

        static string rootPath;
        static string versionReplacementText = "CurrentRedisVersion";

        static void Main(string[] args)
        {

            try
            {
                Program p = new Program();

                string assemblyDirectory = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                rootPath = Path.GetFullPath(Path.Combine(assemblyDirectory, @"..\..\..\..\..\"));

                string version;
                version = p.GetRedisVersion();
                p.UpdateNuSpecFiles(version);
                p.BuildReleasePackage(version, "x64");
                
                Console.Write("Release packaging complete.");
                Environment.ExitCode = 0;
            }
            catch(Exception ex)
            {
                Console.WriteLine("Error. Failed to finish release packaging.\n" + ex.ToString());
                Environment.ExitCode = -1;
            }
        }

        string GetRedisVersion()
        {
            TextReader tr = File.OpenText(Path.Combine(rootPath, @"src\version.h"));
            string line = tr.ReadLine();
            int start = line.IndexOf('\"');
            int last = line.LastIndexOf('\"');
            return line.Substring(start + 1, last - start - 1);
        }

        void ForceFileErase(string file)
        {
            if (File.Exists(file))
            {
                File.Delete(file);
            }
        }

        void CreateTextFileFromTemplate(string templatePath, string documentPath, string toReplace, string replaceWith )
        {
            string replacedText;
            using (TextReader trTemplate = File.OpenText(templatePath) )
            {
                string templateText = trTemplate.ReadToEnd();
                replacedText = templateText.Replace(toReplace, replaceWith);
            }

            ForceFileErase(documentPath);

            using (TextWriter twDoc = File.CreateText(documentPath))
            {
                twDoc.Write(replacedText);
                twDoc.Close();
            }
        }

        void UpdateNuSpecFiles(string version)
        {
            string chocTemplate = Path.Combine(rootPath, @"msvs\setups\chocolatey\template\redis.nuspec.template");
            string chocDocument = Path.Combine(rootPath, @"msvs\setups\chocolatey\redis.nuspec");
            CreateTextFileFromTemplate(chocTemplate, chocDocument, versionReplacementText, version);

            string nugetTemplate = Path.Combine(rootPath, @"msvs\setups\nuget\template\redis.nuspec.template");
            string nugetDocument = Path.Combine(rootPath, @"msvs\setups\nuget\redis.nuspec");
            CreateTextFileFromTemplate(nugetTemplate, nugetDocument, versionReplacementText, version);
        }

        void BuildReleasePackage(string version, string platform)
        {
            string releasePackageDir = Path.Combine(rootPath, @"msvs\BuildRelease\Redis-" + version + @"\");
            if (Directory.Exists(releasePackageDir) == false)
            {
                Directory.CreateDirectory(releasePackageDir);
            }

            string releasePackagePath = Path.Combine(rootPath, releasePackageDir + "Redis-" + platform + "-" + version + ".zip");
            ForceFileErase(releasePackagePath);

            string executablesRoot = Path.Combine(rootPath, @"msvs\" + platform + @"\Release");
            List<string> executableNames = new List<string>()
            {
                "redis-benchmark.exe",
                "redis-check-aof.exe",
                "redis-check-dump.exe",
                "redis-cli.exe",
                "redis-server.exe"
            };
            List<string> symbolNames = new List<string>()
            {
                "redis-benchmark.pdb",
                "redis-check-aof.pdb",
                "redis-check-dump.pdb",
                "redis-cli.pdb",
                "redis-server.pdb"
            };
            List<string> dependencyNames = new List<string>()
            {
                "EventLog.dll"
            };
            string documentsRoot = Path.Combine(rootPath, @"msvs\setups\documentation");
            List<string> docuementNames = new List<string>()
            {
                "Redis on Windows.docx",
                "Redis on Windows Release Notes.docx",
                "Windows Service Documentation.docx",
                "redis.windows.conf",
                "redis.windows-service.conf"
            };

            using (ZipArchive archive = ZipFile.Open(releasePackagePath, ZipArchiveMode.Create))
            {
                foreach (string executableName in executableNames)
                {
                    archive.CreateEntryFromFile(Path.Combine(executablesRoot, executableName), executableName);
                }
                foreach (string symbolName in symbolNames)
                {
                    archive.CreateEntryFromFile(Path.Combine(executablesRoot, symbolName), symbolName);
                }
                foreach (string dependencyName in dependencyNames)
                {
                    archive.CreateEntryFromFile(Path.Combine(executablesRoot, dependencyName), dependencyName);
                }
                foreach (string documentName in docuementNames)
                {
                    archive.CreateEntryFromFile(Path.Combine(documentsRoot, documentName), documentName);
                }
            }
        }

    }
}
