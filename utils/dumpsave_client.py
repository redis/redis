#!/usr/bin/python

"""
NAME

    dumpsaveclient - example implementation of a Redis DUMPSAVE client

SYNOPSIS

    dumpsaveclient HOST PORT FILE

DESCRIPTION

    Connect to the Redis server listening on HOST:PORT and dump database
    index 0 to FILE using the server's native compressed RDB binary
    format.

    Requires Python 2.6.

EXAMPLE USAGE

    % ./dumpsaveclient localhost 6379 dumpsave.rdb
    Dumping RDB from localhost:6379 to dumpsave.rdb...
    18 bytes done

    % src/redis-cli save
    OK

    % crc32 dump.rdb dumpsave.rdb
    504b1d79        dump.rdb
    504b1d79        dumpsave.rdb

"""
import os
import socket
import sys

def dumpsave(redis_server, f):
    """Issue a DUMPSAVE to `redis_server` and write the resulting RDB
    data we get back to `f`.

    `redis_server` must be a `(host, port)` 2-tuple.

    `f` can be anything that behaves like a file.

    """
    BUFLEN = 4096

    # http://redis.io/topics/protocol
    QUERY_DUMPSAVE = "*1\r\n$8\r\nDUMPSAVE\r\n"

    buf = bytearray(BUFLEN)
    s = socket.create_connection(redis_server)

    try:
        s.sendall(QUERY_DUMPSAVE)
        while True:
            received = s.recv_into(buf, BUFLEN)
            if received == 0:
                break
            f.write(buf[0:received])
    finally:
        try:
            s.close()
        except Exception:
            pass

def usage(argv):
    print >>sys.stderr, ("Usage:\n\t%s HOST PORT FILE" %
                         os.path.basename(argv[0]))
    return 2

def main(argv=None):
    if argv == None:
        argv = sys.argv

    try:
        host, port, path = argv[1:4]
    except ValueError:
        return usage(argv)

    print "Dumping RDB from %s:%s to %s..." % (host, port, path)

    f = open(path, 'w')
    try:
        dumpsave((host, port), f)
    finally:
        f.close()

    bytes = os.stat(path).st_size
    print "%d bytes done" % bytes

if __name__ == '__main__':
    sys.exit(main(sys.argv))

