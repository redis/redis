@echo off

echo master
start redis-server master.conf
timeout /T 1 /NOBREAK > NUL

echo slave 1
start redis-server slave.1.conf
timeout /T 1 /NOBREAK > NUL

echo slave 2
start redis-server slave.2.conf
timeout /T 1 /NOBREAK > NUL

echo slave 3
start redis-server slave.3.conf
timeout /T 1 /NOBREAK > NUL

echo sentinal 1
start redis-server sentinel.1.conf --sentinel
timeout /T 1 /NOBREAK > NUL

echo sentinal 2
start redis-server sentinel.2.conf --sentinel
timeout /T 1 /NOBREAK > NUL

echo sentinal 3
start redis-server sentinel.3.conf --sentinel
