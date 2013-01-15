@echo off
netsh advfirewall firewall add rule name="redis" dir=in action=allow enable=yes program="release\\redis-server.exe"