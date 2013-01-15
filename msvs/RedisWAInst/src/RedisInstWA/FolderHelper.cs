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
    using System.Security.AccessControl;
    using System.Text;

    /// <summary>
    /// Utility methods for directories and files
    /// </summary>
    public static class FolderHelper
    {
        /// <summary>
        /// Make a folder if doesn't exist
        /// </summary>
        /// <param name="folderPath"></param>
        /// <returns></returns>
        public static bool MakeFolder(string folderPath)
        {
            if (!Directory.Exists(folderPath))
            {
                try
                {
                    DirectorySecurity securityRules = new DirectorySecurity();
                    securityRules.AddAccessRule(new FileSystemAccessRule("Users", FileSystemRights.FullControl,
                                                                            InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit,
                                                                            PropagationFlags.NoPropagateInherit,
                                                                            AccessControlType.Allow));

                    Directory.CreateDirectory(folderPath, securityRules);
                    File.SetAttributes(folderPath, FileAttributes.Normal);
                }
                catch (Exception ex)
                {
                    Console.WriteLine("Exception trying to create folder: " + folderPath);
                    if (ex.InnerException != null)
                        Console.WriteLine(ex.InnerException.Message);
                    else
                        Console.WriteLine(ex.Message);
                    return false;
                }
            }
            return true;
        }

        public static bool MakeFolderReset(string folderPath)
        {
            if (Directory.Exists(folderPath))
            {
                try
                {
                    // make sure it is empty to start
                    Directory.Delete(folderPath, true);
                }
                catch (Exception ex)
                {
                    Console.WriteLine("Exception trying to delete folder: " + folderPath);
                    if (ex.InnerException != null)
                        Console.WriteLine(ex.InnerException.Message);
                    else
                        Console.WriteLine(ex.Message);
                    return false;
                }
            }

            MakeFolder(folderPath);

            return true;
        }
    }
}
