#!/usr/bin/python

""" redis.py - A client for the Redis daemon.

"""

__author__ = "Ludovico Magnocavallo <ludo\x40qix\x2eit>"
__copyright__ = "Copyright 2009, Ludovico Magnocavallo"
__license__ = "MIT"
__version__ = "0.5"
__revision__ = "$LastChangedRevision: 175 $"[22:-2]
__date__ = "$LastChangedDate: 2009-03-17 16:15:55 +0100 (Mar, 17 Mar 2009) $"[18:-2]


# TODO: Redis._get_multi_response


import socket


BUFSIZE = 4096


class RedisError(Exception): pass
class ConnectionError(RedisError): pass
class ResponseError(RedisError): pass
class InvalidResponse(RedisError): pass
class InvalidData(RedisError): pass


class Redis(object):
    """The main Redis client.
    """
    
    def __init__(self, host=None, port=None, timeout=None, db=None):
        self.host = host or 'localhost'
        self.port = port or 6379
        if timeout:
            socket.setdefaulttimeout(timeout)
        self._sock = None
        self._fp = None
        self.db = db
        
    def _write(self, s):
        """
        >>> r = Redis(db=9)
        >>> r.connect()
        >>> r._sock.close()
        >>> try:
        ...     r._write('pippo')
        ... except ConnectionError, e:
        ...     print e
        Error 9 while writing to socket. Bad file descriptor.
        >>>
        >>> 
        """
        try:
            self._sock.sendall(s)
        except socket.error, e:
            if e.args[0] == 32:
                # broken pipe
                self.disconnect()
            raise ConnectionError("Error %s while writing to socket. %s." % tuple(e.args))
            
    def _read(self):
        try:
            return self._fp.readline()
        except socket.error, e:
            if e.args and e.args[0] == errno.EAGAIN:
                return
            self.disconnect()
            raise ConnectionError("Error %s while reading from socket. %s." % tuple(e.args))
        if not data:
            self.disconnect()
            raise ConnectionError("Socket connection closed when reading.")
        return data
    
    def ping(self):
        """
        >>> r = Redis(db=9)
        >>> r.ping()
        'PONG'
        >>> 
        """
        self.connect()
        self._write('PING\r\n')
        return self.get_response()
    
    def set(self, name, value, preserve=False):
        """
        >>> r = Redis(db=9)
        >>> r.set('a', 'pippo')
        'OK'
        >>> try:
        ...     r.set('a', u'pippo \u3235')
        ... except InvalidData, e:
        ...     print e
        Error encoding unicode value for key 'a': 'ascii' codec can't encode character u'\u3235' in position 15: ordinal not in range(128).
        >>> r.set('b', 105.2)
        'OK'
        >>> r.set('b', 'xxx', preserve=True)
        0
        >>> r.get('b')
        '105.2'
        >>> 
        """
        self.connect()
        # the following will raise an error for unicode values that can't be encoded to ascii
        # we could probably add an 'encoding' arg to init, but then what do we do with get()?
        # convert back to unicode? and what about ints, or pickled values?
        try:
            value = value if isinstance(value, basestring) else str(value)
            self._write('%s %s %s\r\n%s\r\n' % (
                'SETNX' if preserve else 'SET', name, len(value), value
            ))
        except UnicodeEncodeError, e:
            raise InvalidData("Error encoding unicode value for key '%s': %s." % (name, e))
        return self.get_response()
    
    def get(self, name):
        """
        >>> r = Redis(db=9)
        >>> r.set('a', 'pippo'), r.set('b', 15), r.set('c', ' \\r\\naaa\\nbbb\\r\\ncccc\\nddd\\r\\n '), r.set('d', '\\r\\n')
        ('OK', 'OK', 'OK', 'OK')
        >>> r.get('a')
        'pippo'
        >>> r.get('b')
        '15'
        >>> r.get('d')
        '\\r\\n'
        >>> r.get('b')
        '15'
        >>> r.get('c')
        ' \\r\\naaa\\nbbb\\r\\ncccc\\nddd\\r\\n '
        >>> r.get('c')
        ' \\r\\naaa\\nbbb\\r\\ncccc\\nddd\\r\\n '
        >>> r.get('ajhsd')
        >>> 
        """
        self.connect()
        self._write('GET %s\r\n' % name)
        return self.get_response()
    
    def mget(self, *args):
        """
        >>> r = Redis(db=9)
        >>> r.set('a', 'pippo'), r.set('b', 15), r.set('c', '\\r\\naaa\\nbbb\\r\\ncccc\\nddd\\r\\n'), r.set('d', '\\r\\n')
        ('OK', 'OK', 'OK', 'OK')
        >>> r.mget('a', 'b', 'c', 'd')
        ['pippo', '15', '\\r\\naaa\\nbbb\\r\\ncccc\\nddd\\r\\n', '\\r\\n']
        >>> 
        """
        self.connect()
        self._write('MGET %s\r\n' % ' '.join(args))
        return self.get_response()
    
    def incr(self, name, amount=1):
        """
        >>> r = Redis(db=9)
        >>> r.delete('a')
        1
        >>> r.incr('a')
        1
        >>> r.incr('a')
        2
        >>> r.incr('a', 2)
        4
        >>>
        """
        self.connect()
        if amount == 1:
            self._write('INCR %s\r\n' % name)
        else:
            self._write('INCRBY %s %s\r\n' % (name, amount))
        return self.get_response()

    def decr(self, name, amount=1):
        """
        >>> r = Redis(db=9)
        >>> if r.get('a'):
        ...     r.delete('a')
        ... else:
        ...     print 1
        1
        >>> r.decr('a')
        -1
        >>> r.decr('a')
        -2
        >>> r.decr('a', 5)
        -7
        >>> 
        """
        self.connect()
        if amount == 1:
            self._write('DECR %s\r\n' % name)
        else:
            self._write('DECRBY %s %s\r\n' % (name, amount))
        return self.get_response()
    
    def exists(self, name):
        """
        >>> r = Redis(db=9)
        >>> r.exists('dsjhfksjdhfkdsjfh')
        0
        >>> r.set('a', 'a')
        'OK'
        >>> r.exists('a')
        1
        >>>
        """
        self.connect()
        self._write('EXISTS %s\r\n' % name)
        return self.get_response()

    def delete(self, name):
        """
        >>> r = Redis(db=9)
        >>> r.delete('dsjhfksjdhfkdsjfh')
        0
        >>> r.set('a', 'a')
        'OK'
        >>> r.delete('a')
        1
        >>> r.exists('a')
        0
        >>> r.delete('a')
        0
        >>> 
        """
        self.connect()
        self._write('DEL %s\r\n' % name)
        return self.get_response()

    def get_type(self, name):
        """
        >>> r = Redis(db=9)
        >>> r.set('a', 3)
        'OK'
        >>> r.get_type('a')
        'string'
        >>> r.get_type('zzz')
        >>> 
        """
        self.connect()
        self._write('TYPE %s\r\n' % name)
        res = self.get_response()
        return None if res == 'none' else res
    
    def keys(self, pattern):
        """
        >>> r = Redis(db=9)
        >>> r.flush()
        'OK'
        >>> r.set('a', 'a')
        'OK'
        >>> r.keys('a*')
        ['a']
        >>> r.set('a2', 'a')
        'OK'
        >>> r.keys('a*')
        ['a', 'a2']
        >>> r.delete('a2')
        1
        >>> r.keys('sjdfhskjh*')
        []
        >>>
        """
        self.connect()
        self._write('KEYS %s\r\n' % pattern)
        return self.get_response().split()
    
    def randomkey(self):
        """
        >>> r = Redis(db=9)
        >>> r.set('a', 'a')
        'OK'
        >>> isinstance(r.randomkey(), str)
        True
        >>> 
        """
        #raise NotImplementedError("Implemented but buggy, do not use.")
        self.connect()
        self._write('RANDOMKEY\r\n')
        return self.get_response()
    
    def rename(self, src, dst, preserve=False):
        """
        >>> r = Redis(db=9)
        >>> try:
        ...     r.rename('a', 'a')
        ... except ResponseError, e:
        ...     print e
        source and destination objects are the same
        >>> r.rename('a', 'b')
        'OK'
        >>> try:
        ...     r.rename('a', 'b')
        ... except ResponseError, e:
        ...     print e
        no such key
        >>> r.set('a', 1)
        'OK'
        >>> r.rename('b', 'a', preserve=True)
        0
        >>> 
        """
        self.connect()
        if preserve:
            self._write('RENAMENX %s %s\r\n' % (src, dst))
            return self.get_response()
        else:
            self._write('RENAME %s %s\r\n' % (src, dst))
            return self.get_response() #.strip()
        
    def expire(self, name, time):
        """
        >>> r = Redis(db=9)
        >>> r.set('a', 1)
        'OK'
        >>> r.expire('a', 1)
        1
        >>> r.expire('zzzzz', 1)
        0
        >>> 
        """
        self.connect()
        self._write('EXPIRE %s %s\r\n' % (name, time))
        return self.get_response()
    
    def push(self, name, value, tail=False):
        """
        >>> r = Redis(db=9)
        >>> r.delete('l')
        1
        >>> r.push('l', 'a')
        'OK'
        >>> r.set('a', 'a')
        'OK'
        >>> try:
        ...     r.push('a', 'a')
        ... except ResponseError, e:
        ...     print e
        Operation against a key holding the wrong kind of value
        >>> 
        """
        self.connect()
        # same considerations on unicode as in set() apply here
        try:
            value = value if isinstance(value, basestring) else str(value)
            self._write('%s %s %s\r\n%s\r\n' % (
                'LPUSH' if tail else 'RPUSH', name, len(value), value
            ))
        except UnicodeEncodeError, e:
            raise InvalidData("Error encoding unicode value for element in list '%s': %s." % (name, e))
        return self.get_response()
    
    def llen(self, name):
        """
        >>> r = Redis(db=9)
        >>> r.delete('l')
        1
        >>> r.push('l', 'a')
        'OK'
        >>> r.llen('l')
        1
        >>> r.push('l', 'a')
        'OK'
        >>> r.llen('l')
        2
        >>> 
        """
        self.connect()
        self._write('LLEN %s\r\n' % name)
        return self.get_response()

    def lrange(self, name, start, end):
        """
        >>> r = Redis(db=9)
        >>> r.delete('l')
        1
        >>> r.lrange('l', 0, 1)
        []
        >>> r.push('l', 'aaa')
        'OK'
        >>> r.lrange('l', 0, 1)
        ['aaa']
        >>> r.push('l', 'bbb')
        'OK'
        >>> r.lrange('l', 0, 0)
        ['aaa']
        >>> r.lrange('l', 0, 1)
        ['aaa', 'bbb']
        >>> r.lrange('l', -1, 0)
        []
        >>> r.lrange('l', -1, -1)
        ['bbb']
        >>> 
        """
        self.connect()
        self._write('LRANGE %s %s %s\r\n' % (name, start, end))
        return self.get_response()
        
    def ltrim(self, name, start, end):
        """
        >>> r = Redis(db=9)
        >>> r.delete('l')
        1
        >>> try:
        ...     r.ltrim('l', 0, 1)
        ... except ResponseError, e:
        ...     print e
        no such key
        >>> r.push('l', 'aaa')
        'OK'
        >>> r.push('l', 'bbb')
        'OK'
        >>> r.push('l', 'ccc')
        'OK'
        >>> r.ltrim('l', 0, 1)
        'OK'
        >>> r.llen('l')
        2
        >>> r.ltrim('l', 99, 95)
        'OK'
        >>> r.llen('l')
        0
        >>> 
        """
        self.connect()
        self._write('LTRIM %s %s %s\r\n' % (name, start, end))
        return self.get_response()
    
    def lindex(self, name, index):
        """
        >>> r = Redis(db=9)
        >>> res = r.delete('l')
        >>> r.lindex('l', 0)
        >>> r.push('l', 'aaa')
        'OK'
        >>> r.lindex('l', 0)
        'aaa'
        >>> r.lindex('l', 2)
        >>> r.push('l', 'ccc')
        'OK'
        >>> r.lindex('l', 1)
        'ccc'
        >>> r.lindex('l', -1)
        'ccc'
        >>> 
        """
        self.connect()
        self._write('LINDEX %s %s\r\n' % (name, index))
        return self.get_response()
        
    def pop(self, name, tail=False):
        """
        >>> r = Redis(db=9)
        >>> r.delete('l')
        1
        >>> r.pop('l')
        >>> r.push('l', 'aaa')
        'OK'
        >>> r.push('l', 'bbb')
        'OK'
        >>> r.pop('l')
        'aaa'
        >>> r.pop('l')
        'bbb'
        >>> r.pop('l')
        >>> r.push('l', 'aaa')
        'OK'
        >>> r.push('l', 'bbb')
        'OK'
        >>> r.pop('l', tail=True)
        'bbb'
        >>> r.pop('l')
        'aaa'
        >>> r.pop('l')
        >>> 
        """
        self.connect()
        self._write('%s %s\r\n' % ('RPOP' if tail else 'LPOP', name))
        return self.get_response()
    
    def lset(self, name, index, value):
        """
        >>> r = Redis(db=9)
        >>> r.delete('l')
        1
        >>> try:
        ...     r.lset('l', 0, 'a')
        ... except ResponseError, e:
        ...     print e
        no such key
        >>> r.push('l', 'aaa')
        'OK'
        >>> try:
        ...     r.lset('l', 1, 'a')
        ... except ResponseError, e:
        ...     print e
        index out of range
        >>> r.lset('l', 0, 'bbb')
        'OK'
        >>> r.lrange('l', 0, 1)
        ['bbb']
        >>> 
        """
        self.connect()
        try:
            value = value if isinstance(value, basestring) else str(value)
            self._write('LSET %s %s %s\r\n%s\r\n' % (
                name, index, len(value), value
            ))
        except UnicodeEncodeError, e:
            raise InvalidData("Error encoding unicode value for element %s in list '%s': %s." % (index, name, e))
        return self.get_response()
    
    def lrem(self, name, value, num=0):
        """
        >>> r = Redis(db=9)
        >>> r.delete('l')
        1
        >>> r.push('l', 'aaa')
        'OK'
        >>> r.push('l', 'bbb')
        'OK'
        >>> r.push('l', 'aaa')
        'OK'
        >>> r.lrem('l', 'aaa')
        2
        >>> r.lrange('l', 0, 10)
        ['bbb']
        >>> r.push('l', 'aaa')
        'OK'
        >>> r.push('l', 'aaa')
        'OK'
        >>> r.lrem('l', 'aaa', 1)
        1
        >>> r.lrem('l', 'aaa', 1)
        1
        >>> r.lrem('l', 'aaa', 1)
        0
        >>> 
        """
        self.connect()
        try:
            value = value if isinstance(value, basestring) else str(value)
            self._write('LREM %s %s %s\r\n%s\r\n' % (
                name, num, len(value), value
            ))
        except UnicodeEncodeError, e:
            raise InvalidData("Error encoding unicode value for element %s in list '%s': %s." % (index, name, e))
        return self.get_response()
    
    def sort(self, name, by=None, get=None, start=None, num=None, desc=False, alpha=False):
        """
        >>> r = Redis(db=9)
        >>> r.delete('l')
        1
        >>> r.push('l', 'ccc')
        'OK'
        >>> r.push('l', 'aaa')
        'OK'
        >>> r.push('l', 'ddd')
        'OK'
        >>> r.push('l', 'bbb')
        'OK'
        >>> r.sort('l', alpha=True)
        ['aaa', 'bbb', 'ccc', 'ddd']
        >>> r.delete('l')
        1
        >>> for i in range(1, 5):
        ...     res = r.push('l', 1.0 / i)
        >>> r.sort('l')
        ['0.25', '0.333333333333', '0.5', '1.0']
        >>> r.sort('l', desc=True)
        ['1.0', '0.5', '0.333333333333', '0.25']
        >>> r.sort('l', desc=True, start=2, num=1)
        ['0.333333333333']
        >>> r.set('weight_0.5', 10)
        'OK'
        >>> r.sort('l', desc=True, by='weight_*')
        ['0.5', '1.0', '0.333333333333', '0.25']
        >>> for i in r.sort('l', desc=True):
        ...     res = r.set('test_%s' % i, 100 - float(i))
        >>> r.sort('l', desc=True, get='test_*')
        ['99.0', '99.5', '99.6666666667', '99.75']
        >>> r.sort('l', desc=True, by='weight_*', get='test_*')
        ['99.5', '99.0', '99.6666666667', '99.75']
        >>> r.sort('l', desc=True, by='weight_*', get='missing_*')
        [None, None, None, None]
        >>> 
        """
        stmt = ['SORT', name]
        if by:
            stmt.append("BY %s" % by)
        if start and num:
            stmt.append("LIMIT %s %s" % (start, num))
        if get is None:
            pass
        elif isinstance(get, basestring):
            stmt.append("GET %s" % get)
        elif isinstance(get, list) or isinstance(get, tuple):
            for g in get:
                stmt.append("GET %s" % g)
        else:
            raise RedisError("Invalid parameter 'get' for Redis sort")
        if desc:
            stmt.append("DESC")
        if alpha:
            stmt.append("ALPHA")
        self.connect()
        self._write(' '.join(stmt + ["\r\n"]))
        return self.get_response()
    
    def sadd(self, name, value):
        """
        >>> r = Redis(db=9)
        >>> res = r.delete('s')
        >>> r.sadd('s', 'a')
        1
        >>> r.sadd('s', 'b')
        1
        >>> 
        """
        self.connect()
        # same considerations on unicode as in set() apply here
        try:
            value = value if isinstance(value, basestring) else str(value)
            self._write('SADD %s %s\r\n%s\r\n' % (
                name, len(value), value
            ))
        except UnicodeEncodeError, e:
            raise InvalidData("Error encoding unicode value for element in set '%s': %s." % (name, e))
        return self.get_response()
        
    def srem(self, name, value):
        """
        >>> r = Redis(db=9)
        >>> r.delete('s')
        1
        >>> r.srem('s', 'aaa')
        0
        >>> r.sadd('s', 'b')
        1
        >>> r.srem('s', 'b')
        1
        >>> r.sismember('s', 'b')
        0
        >>> 
        """
        self.connect()
        # same considerations on unicode as in set() apply here
        try:
            value = value if isinstance(value, basestring) else str(value)
            self._write('SREM %s %s\r\n%s\r\n' % (
                name, len(value), value
            ))
        except UnicodeEncodeError, e:
            raise InvalidData("Error encoding unicode value for element in set '%s': %s." % (name, e))
        return self.get_response()
    
    def sismember(self, name, value):
        """
        >>> r = Redis(db=9)
        >>> r.delete('s')
        1
        >>> r.sismember('s', 'b')
        0
        >>> r.sadd('s', 'a')
        1
        >>> r.sismember('s', 'b')
        0
        >>> r.sismember('s', 'a')
        1
        >>>
        """
        self.connect()
        # same considerations on unicode as in set() apply here
        try:
            value = value if isinstance(value, basestring) else str(value)
            self._write('SISMEMBER %s %s\r\n%s\r\n' % (
                name, len(value), value
            ))
        except UnicodeEncodeError, e:
            raise InvalidData("Error encoding unicode value for element in set '%s': %s." % (name, e))
        return self.get_response()
    
    def sinter(self, *args):
        """
        >>> r = Redis(db=9)
        >>> res = r.delete('s1')
        >>> res = r.delete('s2')
        >>> res = r.delete('s3')
        >>> r.sadd('s1', 'a')
        1
        >>> r.sadd('s2', 'a')
        1
        >>> r.sadd('s3', 'b')
        1
        >>> try:
        ...     r.sinter()
        ... except ResponseError, e:
        ...     print e
        wrong number of arguments
        >>> try:
        ...     r.sinter('l')
        ... except ResponseError, e:
        ...     print e
        Operation against a key holding the wrong kind of value
        >>> r.sinter('s1', 's2', 's3')
        set([])
        >>> r.sinter('s1', 's2')
        set(['a'])
        >>> 
        """
        self.connect()
        self._write('SINTER %s\r\n' % ' '.join(args))
        return set(self.get_response())
    
    def sinterstore(self, dest, *args):
        """
        >>> r = Redis(db=9)
        >>> res = r.delete('s1')
        >>> res = r.delete('s2')
        >>> res = r.delete('s3')
        >>> r.sadd('s1', 'a')
        1
        >>> r.sadd('s2', 'a')
        1
        >>> r.sadd('s3', 'b')
        1
        >>> r.sinterstore('s_s', 's1', 's2', 's3')
        'OK'
        >>> r.sinterstore('s_s', 's1', 's2')
        'OK'
        >>> r.smembers('s_s')
        set(['a'])
        >>> 
        """
        self.connect()
        self._write('SINTERSTORE %s %s\r\n' % (dest, ' '.join(args)))
        return self.get_response()

    def smembers(self, name):
        """
        >>> r = Redis(db=9)
        >>> r.delete('s')
        1
        >>> r.sadd('s', 'a')
        1
        >>> r.sadd('s', 'b')
        1
        >>> try:
        ...     r.smembers('l')
        ... except ResponseError, e:
        ...     print e
        Operation against a key holding the wrong kind of value
        >>> r.smembers('s')
        set(['a', 'b'])
        >>> 
        """
        self.connect()
        self._write('SMEMBERS %s\r\n' % name)
        return set(self.get_response())

    def select(self, db):
        """
        >>> r = Redis(db=9)
        >>> r.delete('a')
        1
        >>> r.select(10)
        'OK'
        >>> r.set('a', 1)
        'OK'
        >>> r.select(9)
        'OK'
        >>> r.get('a')
        >>> 
        """
        self.connect()
        self._write('SELECT %s\r\n' % db)
        return self.get_response()
    
    def move(self, name, db):
        """
        >>> r = Redis(db=9)
        >>> r.set('a', 'a')
        'OK'
        >>> r.select(10)
        'OK'
        >>> if r.get('a'):
        ...     r.delete('a')
        ... else:
        ...     print 1
        1
        >>> r.select(9)
        'OK'
        >>> r.move('a', 10)
        1
        >>> r.get('a')
        >>> r.select(10)
        'OK'
        >>> r.get('a')
        'a'
        >>> r.select(9)
        'OK'
        >>> 
        """
        self.connect()
        self._write('MOVE %s %s\r\n' % (name, db))
        return self.get_response()
    
    def save(self, background=False):
        """
        >>> r = Redis(db=9)
        >>> r.save()
        'OK'
        >>> try:
        ...     resp = r.save(background=True)
        ... except ResponseError, e:
        ...     assert str(e) == 'background save already in progress', str(e)
        ... else:
        ...     assert resp == 'OK'
        >>> 
        """
        self.connect()
        if background:
            self._write('BGSAVE\r\n')
        else:
            self._write('SAVE\r\n')
        return self.get_response()
        
    def lastsave(self):
        """
        >>> import time
        >>> r = Redis(db=9)
        >>> t = int(time.time())
        >>> r.save()
        'OK'
        >>> r.lastsave() >= t
        True
        >>> 
        """
        self.connect()
        self._write('LASTSAVE\r\n')
        return self.get_response()
    
    def flush(self, all_dbs=False):
        """
        >>> r = Redis(db=9)
        >>> r.flush()
        'OK'
        >>> # r.flush(all_dbs=True)
        >>> 
        """
        self.connect()
        self._write('%s\r\n' % ('FLUSHALL' if all_dbs else 'FLUSHDB'))
        return self.get_response()
    
    def info(self):
        """
        >>> r = Redis(db=9)
        >>> info = r.info()
        >>> info and isinstance(info, dict)
        True
        >>> isinstance(info.get('connected_clients'), int)
        True
        >>> 
        """
        self.connect()
        self._write('INFO\r\n')
        info = dict()
        for l in self.get_response().split('\r\n'):
            if not l:
                continue
            k, v = l.split(':', 1)
            info[k] = int(v) if v.isdigit() else v
        return info
    
    def get_response(self):
        data = self._read().strip()
        c = data[0]
        if c == '-':
            raise ResponseError(data[5:] if data[:5] == '-ERR ' else data[1:])
        if c == '+':
            return data[1:]
        if c == '*':
            try:
                num = int(data[1:])
            except (TypeError, ValueError):
                raise InvalidResponse("Cannot convert multi-response header '%s' to integer" % data)
            result = list()
            for i in range(num):
                result.append(self._get_value())
            return result
        return self._get_value(data)
    
    def _get_value(self, data=None):
        data = data or self._read().strip()
        if data == '$-1':
            return None
        try:
            c, i = data[0], (int(data[1:]) if data.find('.') == -1 else float(data[1:]))
        except ValueError:
            raise InvalidResponse("Cannot convert data '%s' to integer" % data)
        if c == ':':
            return i
        if c != '$':
            raise InvalidResponse("Unkown response prefix for '%s'" % data)
        buf = []
        while True:
            data = self._read()
            i -= len(data)
            buf.append(data)
            if i < 0:
                break
        return ''.join(buf)[:-2]
    
    def disconnect(self):
        if isinstance(self._sock, socket.socket):
            try:
                self._sock.close()
            except socket.error:
                pass
        self._sock = None
        self._fp = None
            
    def connect(self):
        """
        >>> r = Redis(db=9)
        >>> r.connect()
        >>> isinstance(r._sock, socket.socket)
        True
        >>> r.disconnect()
        >>> 
        """
        if isinstance(self._sock, socket.socket):
            return
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect((self.host, self.port))
        except socket.error, e:
            raise ConnectionError("Error %s connecting to %s:%s. %s." % (e.args[0], self.host, self.port, e.args[1]))
        else:
            self._sock = sock
            self._fp = self._sock.makefile('r')
            if self.db:
                self.select(self.db)
                
            
if __name__ == '__main__':
    import doctest
    doctest.testmod()
    
