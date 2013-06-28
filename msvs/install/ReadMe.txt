This solution contains projects for building MSI installer for 32 bit Redis and 64 bit Redis.

The actual binaries to be included in the MSI must be placed in subfolders before building the projects.

Place binaries for the 32 bit Redis installer in folder 'x32'.
Place binaries for the 64 bit Redis installer in folder 'x64'.

The files that should be placed in each of these folders are:
redis-server.exe
redis-cli.exe
redis-check-aof.exe
redis-check-dump.exe
redis.conf
RedisWatcher.exe
RedisWatcher.man
watcher.conf
