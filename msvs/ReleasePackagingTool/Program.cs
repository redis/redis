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
                p.UpdateReleaseNotes(version);
                p.UpdateNuSpecFiles(version);
                p.BuildReleasePackage(version);
                p.DocxToMd();
                Console.Write("Release packaging complete.");
                Environment.ExitCode = 0;
            }
            catch(Exception ex)
            {
                Console.WriteLine("Error. Failed to finish release packaging. \n" + ex.ToString());
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

        void DocxToMd()
        {
            // locate converter (pandoc v1.13.0+) 
            string pandocToolPath = Path.Combine(rootPath, @"msvs\tools\pandoc\pandoc.exe");
            if (File.Exists(pandocToolPath))
            {
                var files = Directory.EnumerateFiles(Path.Combine(rootPath, @"msvs\setups\documentation"), "*.docx");
                foreach (string sourceFile in files)
                {
                    string fileName = Path.GetFileName(sourceFile);
                    string destFile = Path.ChangeExtension(Path.Combine(rootPath, fileName), "md");
                    System.Diagnostics.Process p = new Process();
                    p.StartInfo.FileName = pandocToolPath;
                    p.StartInfo.UseShellExecute = false;
                    p.StartInfo.Arguments = string.Format(
                                                "-f {0} -t {1} -o \"{2}\" \"{3}\"",
                                                "docx",
                                                "markdown_github",
                                                destFile,
                                                sourceFile);
                    p.Start();
                    p.WaitForExit();
                    if (p.ExitCode != 0)
                    {
                        Console.WriteLine("conversion of'{0}' to '{1}' failed.", sourceFile, destFile);
                    }
                }
            }
            else
            {
                Console.WriteLine("pandoc tool not found. docx-->md conversion will not take place.");
            }
        }

        void UpdateReleaseNotes(string redisVersion)
        {
            string releaseNotesPath = Path.Combine(rootPath, @"msvs\setups\documentation\Redis on Windows Release Notes.docx");
            string templatePath = Path.Combine(rootPath, @"msvs\setups\documentation\templates\Redis Release Notes Template.docx");

            ForceFileErase(releaseNotesPath);
            File.Copy(templatePath, releaseNotesPath);

            var archive = ZipFile.Open(releaseNotesPath, ZipArchiveMode.Update);

            string docuemntContent = @"word/document.xml";
            ZipArchiveEntry entry = archive.GetEntry(docuemntContent);
            string updatedContent;
            using (TextReader tr = new StreamReader(entry.Open()))
            {
                string documentContent = tr.ReadToEnd();
                updatedContent = documentContent.Replace(versionReplacementText, redisVersion);
            }
            entry.Delete();
            archive.Dispose();  // forces the file to be written to disk with the documentContent entry deleted 

            archive = System.IO.Compression.ZipFile.Open(releaseNotesPath, ZipArchiveMode.Update);
            ZipArchiveEntry updatedEntry = archive.CreateEntry(docuemntContent, CompressionLevel.Optimal);
            using (TextWriter tw = new StreamWriter(updatedEntry.Open()))
            {
                tw.Write(updatedContent);
            }
            archive.Dispose(); // rewrites the file with the updated content
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

        void UpdateNuSpecFiles(string redisVersion)
        {
            string chocTemplate = Path.Combine(rootPath, @"msvs\setups\chocolatey\template\Redis.nuspec.template");
            string chocDocument = Path.Combine(rootPath, @"msvs\setups\chocolatey\Redis.nuspec");
            CreateTextFileFromTemplate(chocTemplate, chocDocument, versionReplacementText, redisVersion);

            string nugetTemplate = Path.Combine(rootPath, @"msvs\setups\nuget\template\Redis.nuspec.template");
            string nugetDocument = Path.Combine(rootPath, @"msvs\setups\nuget\Redis.nuspec");
            CreateTextFileFromTemplate(nugetTemplate, nugetDocument, versionReplacementText, redisVersion);
        }

        void BuildReleasePackage(string version)
        {
            string releasePackagePath = Path.Combine(rootPath, @"bin\Release\redis-" + version + ".zip");
            ForceFileErase(releasePackagePath);

            string executablesRoot = Path.Combine(rootPath, @"msvs\x64\Release");
            List<string> executableNames = new List<string>()
            {
                "redis-benchmark.exe",
                "redis-check-aof.exe",
                "redis-check-dump.exe",
                "redis-cli.exe",
                "redis-server.exe"
            };
            string documentsRoot = Path.Combine(rootPath, @"msvs\setups\documentation");
            List<string> docuementNames = new List<string>()
            {
                "Redis on Windows.docx",
                "Redis on Windows Release Notes.docx",
                "Windows Service Documentation.docx",
                "redis.windows.conf"
            };

            using (ZipArchive archive = ZipFile.Open(releasePackagePath, ZipArchiveMode.Create))
            {
                foreach (string executableName in executableNames)
                {
                    archive.CreateEntryFromFile(Path.Combine(executablesRoot, executableName), executableName);
                }
                foreach (string documentName in docuementNames)
                {
                    archive.CreateEntryFromFile(Path.Combine(documentsRoot, documentName), documentName);
                }
            }
        }

    }
}
