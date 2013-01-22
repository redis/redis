redis-watcher
=============
RedisWatcher is an application that will run one or more instances of redis-server.
If the redis-server process terminates, then RedisWatcher will restart it.

RedisWatcher is installed as a Windows service.
It can also run as an application by passing 'console' as a command line argument.

Building
========
Visual Studio 2010 was used to create the solution and project files.
The solution includes an installer project that uses WiX Toolset v3.5.
You can download WiX from http://wix.codeplex.com
The RedisWatcher project uses mc.exe to process the RedisWatcher.man to create .h and .rc files


Configuring the watcher
=======================
The watcher.conf file configures RedisWatcher.
The service will load the watcher.conf from the same folder as RedisWatcher.exe.
A sample watcher.conf is provided by the installer.

Specify the location of the redis-server executable to run, as well as the executable file name.
Configure each instance of redis-server to be run.
  - the working directory must be unique per instance
  - the cmdparms is used to specify the configuration file to load if any.
    If running multiple instances, use a Redis configuration file to specify the listening port per instance

If watcher.conf is modified while the service is running, it is reloaded.
  - Any running redis-server processes are not terminated by RedisWatcher.
  - If new redis-server instances are configured, new ones will be started.


Tracing
=======
ETW event messages are defined in RedisWatcher.man.
This is compiled during the build to create .h and .rc files.
The installer modifies the 'messageFileName' and 'resourceFileName' attributes.
The event provider is registered with Windows by the installer.

To manually register the event provider
  wevtutil im RedisWatcher.man

To manually uninstall the event provider
  wevtutil um RedisWatcher.man

To start tracing
  logman start mytrace -p MsOpenTech-RedisWatcher -o mytrace.etl -ets

To stop tracing
  logman stop mytrace -ets

To convert trace output to xml file
  tracerpt mytrace.etl

Other tools are available for formatting the trace.


