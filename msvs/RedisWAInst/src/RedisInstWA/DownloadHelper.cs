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

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.IO;
using System.Net;
using System.Threading;

namespace RedisInstWA
{
    public delegate void Print(String message);

    /// <summary>
    /// Code to download file copied from Inst4WA
    /// </summary>
    public class DownloadHelper : IDisposable
    {
        private AutoResetEvent threadBlocker;
        private int downloadProgress;

        private bool isSuccessful;
        private Exception error;

        public void Download(Uri url, string saveLoc)
        {
            try
            {
                using (WebClient setupDownloader = new WebClient())
                {
                    setupDownloader.DownloadProgressChanged += new DownloadProgressChangedEventHandler(setupDownloader_DownloadProgressChanged);
                    setupDownloader.DownloadFileCompleted += new System.ComponentModel.AsyncCompletedEventHandler(setupDownloader_DownloadFileCompleted);
                    setupDownloader.DownloadFileAsync(url, saveLoc);

                    Console.WriteLine("Downloading - ");
                    threadBlocker = new AutoResetEvent(false);
                    threadBlocker.WaitOne();
                }

                if (isSuccessful == false)
                {
                    throw error;
                }
            }
            finally
            {
                if (threadBlocker != null)
                {
                    threadBlocker.Close();
                    threadBlocker = null;
                }
            }
        }

        private void setupDownloader_DownloadProgressChanged(object sender, DownloadProgressChangedEventArgs e)
        {
            if ((e.ProgressPercentage % 10 == 0) && (e.ProgressPercentage > downloadProgress))
            {
                downloadProgress = e.ProgressPercentage;
                Console.Write(String.Concat(" ", e.ProgressPercentage, "%"));
            }
        }

        private void setupDownloader_DownloadFileCompleted(object sender, System.ComponentModel.AsyncCompletedEventArgs e)
        {
            if (e.Error == null)
            {
                isSuccessful = true;
            }
            else
            {
                isSuccessful = false;
                error = e.Error;
            }

            Console.WriteLine();
            threadBlocker.Set();
        }

        protected virtual void Dispose(bool disposing)
        {
            if (disposing)
            {
                // free managed resources
                if (threadBlocker != null)
                {
                    threadBlocker.Close();
                    threadBlocker = null;
                }
            }
        }

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }
    }
}
