/* redisclient.h -- a C++ client library for redis.
 *
 * Copyright (c) 2009, Brian Hammond <brian at fictorial dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef REDISCLIENT_H
#define REDISCLIENT_H

#include <string>
#include <vector>
#include <set>
#include <stdexcept>
#include <ctime>

namespace redis 
{
  struct server_info 
  {
    std::string version;
    bool bgsave_in_progress;
    unsigned long connected_clients;
    unsigned long connected_slaves;
    unsigned long used_memory;
    unsigned long changes_since_last_save;
    unsigned long last_save_time;
    unsigned long total_connections_received;
    unsigned long total_commands_processed;
    unsigned long uptime_in_seconds;
    unsigned long uptime_in_days;
  };

  // Generic error that is thrown when communicating with the redis server.

  class redis_error 
  {
  public:
    redis_error(const std::string & err);
    operator std::string ();
    operator const std::string () const;
  private:
    std::string err_;
  };

  // Some socket-level I/O or general connection error.

  class connection_error : public redis_error
  {
  public:
    connection_error(const std::string & err);
  };

  // Redis gave us a reply we were not expecting.
  // Possibly an internal error (here or in redis, probably here).

  class protocol_error : public redis_error
  {
  public:
    protocol_error(const std::string & err);
  };

  // A key that you expected to exist does not in fact exist.

  class key_error : public redis_error
  {
  public:
    key_error(const std::string & err);
  };

  // A value of an expected type or other semantics was found to be invalid.

  class value_error : public redis_error
  {
  public:
    value_error(const std::string & err);
  };

  // You should construct a 'client' object per connection to a redis-server.
  //
  // Please read the online redis command reference:
  // http://code.google.com/p/redis/wiki/CommandReference
  //
  // No provisions for customizing the allocator on the string/bulk value type
  // (std::string) are provided.  If needed, you can always change the
  // string_type typedef in your local version.

  class client
  {
  public:
    typedef std::string string_type;
    typedef std::vector<string_type> string_vector;
    typedef std::set<string_type> string_set;

    typedef long int_type;

    explicit client(const string_type & host = "localhost", 
                    unsigned int port = 6379);

    ~client();

    //
    // Connection handling
    // 

    void auth(const string_type & pass);

    //
    // Commands operating on string values
    //
    // Note that empty string values do not denote nonexistent keys but well,
    // empty values!  If a nonexistent key is queried, the value returned will
    // be missing_value, including when string_vector objects are returned.
    //

    static string_type missing_value;

    // set a key to a string value

    void set(const string_type & key, const string_type & value);

    // return the string value of the key

    string_type get(const string_type & key);

    // set a key to a string returning the old value of the key

    string_type getset(const string_type & key, const string_type & value);

    // multi-get, return the strings values of the keys

    void mget(const string_vector & keys, string_vector & out);

    // set a key to a string value if the key does not exist.  returns true if
    // the key was set, else false.  This does not throw since you are ok with
    // this failing if the dst key already exists.

    bool setnx(const string_type & key, const string_type & value);

    // increment the integer value of key
    // returns new value

    int_type incr(const string_type & key);

    // increment the integer value of key by integer
    // returns new value

    int_type incrby(const string_type & key, int_type by);

    // decrement the integer value of key
    // returns new value

    int_type decr(const string_type & key);

    // decrement the integer value of key by integer
    // returns new value

    int_type decrby(const string_type & key, int_type by);

    // test if a key exists

    bool exists(const string_type & key);

    // delete a key
    // throws if doesn't exist

    void del(const string_type & key);

    enum datatype 
    {
      datatype_none,      // key doesn't exist
      datatype_string,
      datatype_list,
      datatype_set
    };

    // return the type of the value stored at key

    datatype type(const string_type & key);

    //
    // Commands operating on the key space
    //

    // find all the keys matching a given pattern
    // returns numbers of keys appended to 'out'

    int_type keys(const string_type & pattern, string_vector & out);

    // return a random key from the key space
    // returns empty string if db is empty

    string_type randomkey();

    // rename the old key in the new one, destroying the new key if 
    // it already exists

    void rename(const string_type & old_name, const string_type & new_name);

    // rename the old key in the new one, if the new key does not already
    // exist.  This does not throw since you are ok with this failing if the
    // new_name key already exists.

    bool renamenx(const string_type & old_name, const string_type & new_name);

    // return the number of keys in the current db

    int_type dbsize();

    // set a time to live in seconds on a key.  
    // fails if there's already a timeout on the key.
    
    // NB: there's currently no generic way to remove a timeout on a key

    void expire(const string_type & key, unsigned int secs);

    //
    // Commands operating on lists
    //

    // Append an element to the tail of the list value at key

    void rpush(const string_type & key, const string_type & value);

    // Append an element to the head of the list value at key

    void lpush(const string_type & key, const string_type & value);

    // Return the length of the list value at key
    // Returns 0 if the list does not exist; see 'exists'

    int_type llen(const string_type & key);

    // Fetch a range of elements from the list at key
    // end can be negative for reverse offsets
    // Returns number of elements appended to 'out'

    int_type lrange(const string_type & key, 
                    int_type start, 
                    int_type end,
                    string_vector & out);

    // Fetches the entire list at key.

    int_type get_list(const string_type & key, string_vector & out)
    {
      return lrange(key, 0, -1, out);
    }

    // Trim the list at key to the specified range of elements

    void ltrim(const string_type & key, int_type start, int_type end);

    // Return the element at index position from the list at key

    string_type lindex(const string_type & key, int_type);

    // set a new value as the element at index position of the list at key

    void lset(const string_type & key, 
              int_type index, 
              const string_type &);

    // If count is zero all the elements are removed. If count is negative
    // elements are removed from tail to head, instead to go from head to tail
    // that is the normal behaviour. So for example LREM with count -2 and
    // hello as value to remove against the list (a,b,c,hello,x,hello,hello)
    // will lave the list (a,b,c,hello,x). Returns the number of removed
    // elements if the operation succeeded. 
    //
    // Note: this will not throw if the number of elements removed != count
    // since you might want to remove at most count elements by don't care if
    // < count elements are removed.  See lrem_exact().

    int_type lrem(const string_type & key, 
                  int_type count, 
                  const string_type & value);

    // An extension of 'lrem' that wants to remove exactly 'count' elements.
    // Throws value_error if 'count' elements are not found & removed from the
    // list at 'key'.

    void lrem_exact(const string_type & key,
                    int_type count,
                    const string_type & value)
    { 
      if (lrem(key, count, value) != count)
        throw value_error("failed to remove exactly N elements from list");
    }

    // Return and remove (atomically) the first element of the list at key

    string_type lpop(const string_type & key);

    // Return and remove (atomically) the last element of the list at key

    string_type rpop(const string_type & key);

    //
    // Commands operating on sets
    //

    // Add the specified member to the set value at key
    // returns true if added, or false if already a member of the set.

    void sadd(const string_type & key, const string_type & value);

    // Remove the specified member from the set value at key
    // returns true if removed or false if value is not a member of the set.

    void srem(const string_type & key, const string_type & value);

    // Move the specified member from one set to another atomically
    // returns true if element was moved, else false (e.g. not found)

    void smove(const string_type & srckey, 
               const string_type & dstkey, 
               const string_type & value);

    // Return the number of elements (the cardinality) of the set at key

    int_type scard(const string_type & key);

    // Test if the specified value is a member of the set at key
    // Returns false if key doesn't exist or value is not a member of the set at key

    bool sismember(const string_type & key, const string_type & value);

    // Return the intersection between the sets stored at key1, key2, ..., keyN

    int_type sinter(const string_vector & keys, string_set & out);

    // Compute the intersection between the sets stored at key1, key2, ..., 
    // keyN, and store the resulting set at dstkey

    void sinterstore(const string_type & dstkey, const string_vector & keys);

    // Return the union between the sets stored at key1, key2, ..., keyN

    int_type sunion(const string_vector & keys, string_set & out);

    // Compute the union between the sets stored at key1, key2, ..., keyN, 
    // and store the resulting set at dstkey

    void sunionstore(const string_type & dstkey, const string_vector & keys);

    // Return all the members of the set value at key

    int_type smembers(const string_type & key, string_set & out);

    //
    // Multiple databases handling commands
    //

    // Select the DB having the specified index

    void select(int_type dbindex);

    // Move the key from the currently selected DB to the DB having as index
    // dbindex.  Throws if key was already in the db at dbindex or not found in
    // currently selected db.

    void move(const string_type & key, int_type dbindex);

    // Remove all the keys of the currently selected DB

    void flushdb();

    // Remove all the keys from all the databases

    void flushall();

    //
    // Sorting
    // Just go read http://code.google.com/p/redis/wiki/SortCommand
    //

    enum sort_order
    {
      sort_order_ascending,
      sort_order_descending
    };

    int_type sort(const string_type & key, 
                  string_vector & out,
                  sort_order order = sort_order_ascending,
                  bool lexicographically = false);

    int_type sort(const string_type & key, 
                  string_vector & out,
                  int_type limit_start, 
                  int_type limit_end, 
                  sort_order order = sort_order_ascending,
                  bool lexicographically = false);

    int_type sort(const string_type & key, 
                  string_vector & out,
                  const string_type & by_pattern, 
                  int_type limit_start, 
                  int_type limit_end, 
                  const string_vector & get_patterns, 
                  sort_order order = sort_order_ascending,
                  bool lexicographically = false);

    //
    // Persistence control commands
    //

    // Synchronously save the DB on disk

    void save();

    // Asynchronously save the DB on disk

    void bgsave();

    // Return the UNIX time stamp of the last successfully saving of the 
    // dataset on disk

    time_t lastsave();

    // Synchronously save the DB on disk, then shutdown the server.  This
    // object's connection to the server will be lost on success.  Otherwise,
    // redis_error is raised.  Thus, on success, you should delete or otherwise
    // no longer use the object.

    void shutdown();

    //
    // Remote server control commands
    //

    // Provide information and statistics about the server

    void info(server_info & out);

  private:
    client(const client &);
    client & operator=(const client &);

    void        send_(const std::string &);
    void        recv_ok_reply_();
    void        recv_int_ok_reply_();
    std::string recv_single_line_reply_();
    int_type    recv_bulk_reply_(char prefix);
    std::string recv_bulk_reply_();
    int_type    recv_multi_bulk_reply_(string_vector & out);
    int_type    recv_multi_bulk_reply_(string_set & out);
    int_type    recv_int_reply_();

  private:
    int socket_;
  };
}

#endif
