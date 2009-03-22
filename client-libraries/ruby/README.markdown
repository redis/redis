# redis-rb

A ruby client library for the redis key value storage system.

## Information about redis

Redis is a key value store with some interesting features:
1. It's fast.
2. Keys are strings but values can have types of "NONE", "STRING", "LIST",  or "SET".  List's can be atomically push'd, pop'd, lpush'd, lpop'd and indexed.  This allows you to store things like lists of comments under one key while retaining the ability to append comments without reading and putting back the whole list.

See [redis on code.google.com](http://code.google.com/p/redis/wiki/README) for more information.

## Dependencies

1. redis - 

		rake redis:install

2. dtach - 

		rake dtach:install

3. svn - git is the new black, but we need it for the google codes.

## Setup

Use the tasks mentioned above (in Dependencies) to get your machine setup.

## Examples

Check the examples/ directory.  *Note* you need to have redis-server running first.