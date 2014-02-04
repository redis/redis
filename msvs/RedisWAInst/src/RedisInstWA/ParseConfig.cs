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
    using System.Linq;
    using System.Text;
    using System.Xml.XPath;
    using System.Xml;

    /// <summary>
    /// Parse the csdef file to find roles to be installed and ports
    /// </summary>
    public class ParseConfig
    {
        public static List<RedisInstance> Parse(string filePath)
        {
            List<RedisInstance> instances = new List<RedisInstance>();

            try
            {
                XPathDocument csdef = new XPathDocument(filePath);
                XPathNavigator csdefNav = csdef.CreateNavigator();
                XmlNamespaceManager nsm = new XmlNamespaceManager(csdefNav.NameTable);

                nsm.AddNamespace("ns", "http://schemas.microsoft.com/ServiceHosting/2008/10/ServiceDefinition");

                XPathNodeIterator roles = csdefNav.Select("/ns:ServiceDefinition/ns:WorkerRole", nsm);

                // first node must be master
                bool isMaster = true;
                int masterPort = 0;
                while (roles.MoveNext())
                {
                    int port = 0;
                    string localPort = string.Empty;
                    string extPort = string.Empty;
                    string roleName = roles.Current.GetAttribute("name", string.Empty);
                    XPathNodeIterator endpoint = roles.Current.Select("./ns:Endpoints/ns:InputEndpoint", nsm);
                    if (endpoint.MoveNext())
                    {
                        localPort = endpoint.Current.GetAttribute("localPort", string.Empty);
                        extPort = endpoint.Current.GetAttribute("port", string.Empty);
                    }

                    if (roleName != string.Empty && localPort != string.Empty)
                    {
                        int.TryParse(localPort, out port);
                        RedisInstance instance = new RedisInstance();
                        instance.IsMaster = isMaster;
                        instance.LocalPort = port;
                        instance.RoleName = roleName;
                        if (isMaster)
                        {
                            if (int.TryParse(extPort, out port))
                                masterPort = port;
                        }
                        instance.MasterPort = masterPort;
                        instances.Add(instance);
                        isMaster = false;
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine("Parsing ServiceDefinition file failed: " + filePath);
                if (ex.InnerException != null)
                    Console.WriteLine(ex.InnerException.Message);
                else
                    Console.WriteLine(ex.Message);
            }

            return instances;
        }

    }
}
