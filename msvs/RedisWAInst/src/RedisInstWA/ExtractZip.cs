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
    using Shell32;
    using System;
    using System.Collections.Generic;
    using System.IO;
    using System.Linq;
    using System.Text;

    /// <summary>
    /// Download zip from URL and extract files
    /// </summary>
    public class ExtractZip
    {
        public void Download(string url, string path)
        {
            DownloadHelper down = new DownloadHelper();
            Uri uri = new Uri(url);
            down.Download(uri, path);
        }

        public string FindBinZip(string path, string binzip)
        {
            IEnumerable<string> subdirs = Directory.EnumerateDirectories(path);
            if (subdirs != null && subdirs.Count() > 0)
            {
                string subpath = Path.Combine(path, subdirs.First());
                string binpath = Path.Combine(subpath, @"msvs\bin\release\", binzip);
                if (File.Exists(binpath))
                {
                    return binpath;
                }
                else
                {
                    // bin folder moved in later versions.
                    binpath = Path.Combine(subpath, @"bin\release\", binzip);
                    if (File.Exists(binpath))
                    {
                        return binpath;
                    }
                }
            }

            return null;
        }

        public void ExtractFilesFromZip(string src, string dest)
        {
            Shell sh = new Shell();
            Folder inzip = sh.NameSpace(src);
            Folder outdir = sh.NameSpace(dest);
            outdir.CopyHere(inzip.Items(), 0);  // 4 for no progress dialog, 16 for Yes to all
        }

        public int DownloadAndExtract(string zipurl, string workPath, string binfolder, bool isX64)
        {
            string zippath = Path.Combine(workPath, "zipfile");
            if (!FolderHelper.MakeFolderReset(zippath))
                return 5;

            try
            {
                string zipfile = Path.Combine(zippath, "redis.zip");
                Download(zipurl, zipfile);
                IEnumerable<string> subfiles = Directory.EnumerateFiles(zippath);
                if (!File.Exists(zipfile))
                {
                    // download did nothing
                    Console.WriteLine("Downloading zip file failed");
                    return 1;
                }

                // unzip the downloaded file
                ExtractFilesFromZip(zipfile, zippath);

                string binzip = isX64 ? "redisbin64.zip" : "redisbin.zip";
                string embeddedzip = FindBinZip(zippath, binzip);
                if (embeddedzip == null)
                {
                    // binaries zip not found
                    Console.WriteLine("Did not find {0} in downloaded zip file", binzip);
                    return 2;
                }
                string binpath = Path.Combine(workPath, binfolder);
                if (!FolderHelper.MakeFolderReset(binpath))
                    return 5;

                ExtractFilesFromZip(embeddedzip, binpath);

                IEnumerable<string> exefiles = Directory.EnumerateFiles(binpath, "redis-server.exe", SearchOption.AllDirectories);
                if (exefiles == null || exefiles.Count() == 0)
                {
                    // executable not found
                    Console.WriteLine("Extracting redis-server from zip failed");
                    return 3;
                }
            }
            catch (Exception ex)
            {
                if (ex.InnerException != null)
                    Console.WriteLine(ex.InnerException.Message);
                else
                    Console.WriteLine(ex.Message);
                return 4;
            }

            return 0;
        }
    }
}
