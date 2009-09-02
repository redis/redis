# Redis Scala client

## Key features of the library

- Native Scala types Set and List responses.
- Consisten Hashing on the client.
- Support for Clustering of Redis nodes.

## Information about redis

Redis is a key-value database. It is similar to memcached but the dataset is not volatile, and values can be strings, exactly like in memcached, but also lists and sets with atomic operations to push/pop elements.

http://code.google.com/p/redis/

### Key features of Redis

- Fast in-memory store with asynchronous save to disk.
- Key value get, set, delete, etc.
- Atomic operations on sets and lists, union, intersection, trim, etc.

## Requirements

- sbt (get it at http://code.google.com/p/simple-build-tool/)

## Usage

Start your redis instance (usually redis-server will do it)

    $ cd scala-redis
    $ sbt
    > update
    > test (optional to run the tests)
    > console

And you are ready to start issuing commands to the server(s)

let's connect and get a key:

    scala> import com.redis._
    scala> val r = new Redis("localhost", 6379)
    scala> val r.set("key", "some value")
    scala> val r.get("key")


Alejandro Crosa <<alejandrocrosa@gmail.com>>

