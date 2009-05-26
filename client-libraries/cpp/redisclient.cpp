/* redisclient.cpp -- a C++ client library for redis.
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

#include "redisclient.h"
#include "anet.h"

#include <sstream>

#ifndef NDEBUG
#include <algorithm>
#include <iostream>
#include <ctime>
#endif

#include <cstring>
#include <cstdlib>
#include <cassert>

#include <sys/errno.h>
#include <sys/socket.h>

using namespace std;

namespace 
{
  const string whitespace(" \f\n\r\t\v");
  const string CRLF("\r\n");

  // Modifies in-place.

  inline string & rtrim(string & str, const string & ws = whitespace)
  {
    string::size_type pos = str.find_last_not_of(ws);
    str.erase(pos + 1);
    return str;
  }

  vector<string>::size_type split(const string & str, char delim, vector<string> & elems)
  {
    stringstream ss(str);
    string item;
    vector<string>::size_type n = 0;
    while (getline(ss, item, delim)) 
    {
      elems.push_back(item); 
      ++n;
    }
    return n;
  }

  inline void split_lines(const string & str, vector<string> & elems) 
  {
    split(str, '\n', elems);
    for (vector<string>::iterator it = elems.begin(); it != elems.end(); ++it)
      rtrim(*it);
  }

#ifndef NDEBUG

  void output_proto_debug(const string & data, bool is_received = true)
  {
    string escaped_data(data);
    size_t pos;
    while ((pos = escaped_data.find("\n")) != string::npos)
      escaped_data.replace(pos, 1, "\\n");
    while ((pos = escaped_data.find("\r")) != string::npos)
      escaped_data.replace(pos, 1, "\\r");

    cerr 
      << time(NULL) << ": " 
      << (is_received ? "RECV '" : "SEND '")
      << escaped_data 
      << "'" 
      << endl;
  }

#endif

  class makecmd
  {
  public:
    explicit makecmd(const string & initial, bool finalize = false) 
    {
      buffer_ << initial;
      if (!finalize)
        buffer_ << " ";
    }

    template <typename T> 
    makecmd & operator<<(T const & datum)
    {
      buffer_ << datum;
      return *this;
    }

    template <typename T>
    makecmd & operator<<(const vector<T> & data) 
    {
      size_t n = data.size();
      for (size_t i = 0; i < n; ++i)
      {
        buffer_ << data[i];
        if (i < n - 1)
          buffer_ << " ";
      }
      return *this;
    }

    operator std::string ()
    {
      buffer_ << CRLF;
      return buffer_.str();
    }

  private:
    ostringstream buffer_;
  };

  // Reads N bytes from given blocking socket.

  string read_n(int socket, ssize_t n)
  {
    char * buffer = new char[n + 1];
    buffer[n] = '\0';

    char * bp = buffer;
    ssize_t bytes_read = 0;

    while (bytes_read != n) 
    {
      ssize_t bytes_received = 0;
      do bytes_received = recv(socket, bp, n - (bp - buffer), 0);
      while (bytes_received < 0 && errno == EINTR);

      if (bytes_received == 0)
        throw redis::connection_error("connection was closed");

      bytes_read += bytes_received;
      bp         += bytes_received;
    }

    string str(buffer);
    delete [] buffer;
    return str;
  }

  // Reads a single line of character data from the given blocking socket.
  // Returns the line that was read, not including EOL delimiter(s).  Both LF
  // ('\n') and CRLF ("\r\n") delimiters are supported.  If there was an I/O
  // error reading from the socket, connection_error is raised.  If max_size
  // bytes are read before finding an EOL delimiter, a blank string is
  // returned.

  string read_line(int socket, ssize_t max_size = 2048) 
  {
    assert(socket > 0);
    assert(max_size > 0);

    ostringstream oss;

    enum { buffer_size = 64 };
    char buffer[buffer_size];
    memset(buffer, 0, buffer_size);

    ssize_t total_bytes_read = 0;
    bool found_delimiter = false;

    while (total_bytes_read < max_size && !found_delimiter)
    {
      // Peek at what's available.

      ssize_t bytes_received = 0;
      do bytes_received = recv(socket, buffer, buffer_size, MSG_PEEK);
      while (bytes_received < 0 && errno == EINTR);

      if (bytes_received == 0)
        throw redis::connection_error("connection was closed");

      // Some data is available; Length might be < buffer_size.
      // Look for newline in whatever was read though.

      char * eol = static_cast<char *>(memchr(buffer, '\n', bytes_received));

      // If found, write data from the buffer to the output string.
      // Else, write the entire buffer and continue reading more data.

      ssize_t to_read = bytes_received;

      if (eol) 
      {
        to_read = eol - buffer + 1;
        oss.write(buffer, to_read);
        found_delimiter = true;
      }
      else
        oss.write(buffer, bytes_received);

      // Now read from the socket to remove the peeked data from the socket's
      // read buffer.  This will not block since we've peeked already and know
      // there's data waiting.  It might fail if we were interrupted however.

      do bytes_received = recv(socket, buffer, to_read, 0);
      while (bytes_received < 0 && errno == EINTR);
    }

    // Construct final line string. Remove trailing CRLF-based whitespace.

    string line = oss.str();
    return rtrim(line, CRLF);
  }

  unsigned long unsigned_number_from_string(const string & data)
  {
    errno = 0;

    unsigned long value = strtoul(data.c_str(), NULL, 10);

    if (value == ULONG_MAX && errno == ERANGE)
      throw redis::value_error("invalid number; out of range of long");

    if (value == 0 && errno == EINVAL)
      throw redis::value_error("invalid number; unrecognized format");

    return value;
  }

  redis::client::int_type number_from_string(const string & data)
  {
    errno = 0;

    redis::client::int_type value = strtol(data.c_str(), NULL, 10);

    if ((value == LONG_MAX || value == LONG_MIN) && errno == ERANGE)
      throw redis::value_error("invalid number; out of range of long");

    if (value == 0 && errno == EINVAL)
      throw redis::value_error("invalid number; unrecognized format");

    return value;
  }

  const string status_reply_ok("OK");
  const string prefix_status_reply_error("-ERR ");
  const char prefix_status_reply_value = '+';
  const char prefix_single_bulk_reply = '$';
  const char prefix_multi_bulk_reply = '*';
  const char prefix_int_reply = ':';

  const string server_info_key_version = "redis_version";
  const string server_info_key_bgsave_in_progress = "bgsave_in_progress";
  const string server_info_key_connected_clients = "connected_clients";
  const string server_info_key_connected_slaves = "connected_slaves";
  const string server_info_key_used_memory = "used_memory";
  const string server_info_key_changes_since_last_save = "changes_since_last_save";
  const string server_info_key_last_save_time = "last_save_time";
  const string server_info_key_total_connections_received = "total_connections_received";
  const string server_info_key_total_commands_processed = "total_commands_processed";
  const string server_info_key_uptime_in_seconds = "uptime_in_seconds";
  const string server_info_key_uptime_in_days = "uptime_in_days";
}

namespace redis 
{
  redis_error::redis_error(const string & err) : err_(err) 
  {
  }

  redis_error::operator std::string ()
  {
    return err_;
  }

  redis_error::operator const std::string () const
  {
    return err_;
  }

  connection_error::connection_error(const string & err) : redis_error(err)
  {
  }

  protocol_error::protocol_error(const string & err) : redis_error(err)
  {
  }

  key_error::key_error(const string & err) : redis_error(err)
  {
  }

  value_error::value_error(const string & err) : redis_error(err)
  {
  }

  client::string_type client::missing_value("**nonexistent-key**");

  client::client(const string_type & host, unsigned int port)
  {
    char err[ANET_ERR_LEN];
    socket_ = anetTcpConnect(err, const_cast<char*>(host.c_str()), port);
    if (socket_ == ANET_ERR) 
      throw connection_error(err);
    anetTcpNoDelay(NULL, socket_);
  }

  client::~client()
  {
    if (socket_ != ANET_ERR)
      close(socket_);
  }

  void client::auth(const client::string_type & pass)
  {
    send_(makecmd("AUTH") << pass);
    recv_ok_reply_();
  }

  void client::set(const client::string_type & key, 
                   const client::string_type & value)
  {
    send_(makecmd("SET") << key << ' ' << value.size() << CRLF << value);
    recv_ok_reply_();
  }

  client::string_type client::get(const client::string_type & key)
  {
    send_(makecmd("GET") << key);
    return recv_bulk_reply_();
  }

  client::string_type client::getset(const client::string_type & key, 
                                     const client::string_type & value)
  {
    send_(makecmd("GETSET") << key << ' ' << value.size() << CRLF << value);
    return recv_bulk_reply_();
  }

  void client::mget(const client::string_vector & keys, string_vector & out)
  {
    send_(makecmd("MGET") << keys);
    recv_multi_bulk_reply_(out);
  }

  bool client::setnx(const client::string_type & key, 
                     const client::string_type & value)
  {
    send_(makecmd("SETNX") << key << ' ' << value.size() << CRLF << value);
    return recv_int_reply_() == 1;
  }

  client::int_type client::incr(const client::string_type & key)
  {
    send_(makecmd("INCR") << key);
    return recv_int_reply_();
  }

  client::int_type client::incrby(const client::string_type & key, 
                                      client::int_type by)
  {
    send_(makecmd("INCRBY") << key << ' ' << by);
    return recv_int_reply_();
  }

  client::int_type client::decr(const client::string_type & key)
  {
    send_(makecmd("DECR") << key);
    return recv_int_reply_();
  }

  client::int_type client::decrby(const client::string_type & key, 
                                      client::int_type by)
  {
    send_(makecmd("DECRBY") << key << ' ' << by);
    return recv_int_reply_();
  }

  bool client::exists(const client::string_type & key)
  {
    send_(makecmd("EXISTS") << key);
    return recv_int_reply_() == 1;
  }

  void client::del(const client::string_type & key)
  {
    send_(makecmd("DEL") << key);
    recv_int_ok_reply_();
  }

  client::datatype client::type(const client::string_type & key)
  {
    send_(makecmd("TYPE") << key);
    string response = recv_single_line_reply_();

    if (response == "none")   return datatype_none;
    if (response == "string") return datatype_string;
    if (response == "list")   return datatype_list;
    if (response == "set")    return datatype_set;

    return datatype_none;
  }

  client::int_type client::keys(const client::string_type & pattern,
                                 client::string_vector & out)
  {
    send_(makecmd("KEYS") << pattern);
    string resp = recv_bulk_reply_();
    return split(resp, ' ', out);
  }

  client::string_type client::randomkey()
  {
    send_(makecmd("RANDOMKEY", true));
    return recv_single_line_reply_();
  }

  void client::rename(const client::string_type & old_name, 
                      const client::string_type & new_name)
  {
    send_(makecmd("RENAME") << old_name << ' ' << new_name);
    recv_ok_reply_();
  }

  bool client::renamenx(const client::string_type & old_name, 
                        const client::string_type & new_name)
  {
    send_(makecmd("RENAMENX") << old_name << ' ' << new_name);
    return recv_int_reply_() == 1;
  }

  client::int_type client::dbsize()
  {
    send_(makecmd("DBSIZE"));
    return recv_int_reply_();
  }

  void client::expire(const string_type & key, unsigned int secs)
  {
    send_(makecmd("EXPIRE") << key << ' ' << secs);
    recv_int_ok_reply_();
  }

  void client::rpush(const client::string_type & key, 
                     const client::string_type & value)
  {
    send_(makecmd("RPUSH") << key << ' ' << value.length() << CRLF << value);
    recv_ok_reply_();
  }

  void client::lpush(const client::string_type & key, 
                     const client::string_type & value)
  {
    send_(makecmd("LPUSH") << key << ' ' << value.length() << CRLF << value);
    recv_ok_reply_();
  }

  client::int_type client::llen(const client::string_type & key)
  {
    send_(makecmd("LLEN") << key);
    return recv_int_reply_();
  }

  client::int_type client::lrange(const client::string_type & key, 
                                   client::int_type start, 
                                   client::int_type end,
                                   client::string_vector & out)
  {
    send_(makecmd("LRANGE") << key << ' ' << start << ' ' << end);
    return recv_multi_bulk_reply_(out);
  }

  void client::ltrim(const client::string_type & key, 
                     client::int_type start, 
                     client::int_type end)
  {
    send_(makecmd("LTRIM") << key << ' ' << start << ' ' << end);
    recv_ok_reply_();
  }

  client::string_type client::lindex(const client::string_type & key, 
                                     client::int_type index)
  {
    send_(makecmd("LINDEX") << key << ' ' << index);
    return recv_bulk_reply_();
  }

  void client::lset(const client::string_type & key, 
                    client::int_type index, 
                    const client::string_type & value)
  {
    send_(makecmd("LSET") << key << ' ' << index << ' ' << value.length() << CRLF << value);
    recv_ok_reply_();
  }

  client::int_type client::lrem(const client::string_type & key, 
                                client::int_type count, 
                                const client::string_type & value)
  {
    send_(makecmd("LREM") << key << ' ' << count << ' ' << value.length() << CRLF << value);
    return recv_int_reply_();
  }

  client::string_type client::lpop(const client::string_type & key)
  {
    send_(makecmd("LPOP") << key);
    return recv_bulk_reply_();
  }

  client::string_type client::rpop(const client::string_type & key)
  {
    send_(makecmd("RPOP") << key);
    return recv_bulk_reply_();
  }

  void client::sadd(const client::string_type & key, 
                    const client::string_type & value)
  {
    send_(makecmd("SADD") << key << ' ' << value.length() << CRLF << value);
    recv_int_ok_reply_();
  }

  void client::srem(const client::string_type & key, 
                    const client::string_type & value)
  {
    send_(makecmd("SREM") << key << ' ' << value.length() << CRLF << value);
    recv_int_ok_reply_();
  }

  void client::smove(const client::string_type & srckey, 
                     const client::string_type & dstkey, 
                     const client::string_type & value)
  {
    send_(makecmd("SMOVE") << srckey << ' ' << dstkey << ' ' << value.length() << CRLF << value);
    recv_int_ok_reply_();
  }

  client::int_type client::scard(const client::string_type & key)
  {
    send_(makecmd("SCARD") << key);
    return recv_int_reply_();
  }

  bool client::sismember(const client::string_type & key, 
                         const client::string_type & value)
  {
    send_(makecmd("SISMEMBER") << key << ' ' << value.length() << CRLF << value);
    return recv_int_reply_() == 1;
  }

  client::int_type client::sinter(const client::string_vector & keys, client::string_set & out)
  {
    send_(makecmd("SINTER") << keys);
    return recv_multi_bulk_reply_(out);
  }

  void client::sinterstore(const client::string_type & dstkey, 
                           const client::string_vector & keys)
  {
    send_(makecmd("SINTERSTORE") << dstkey << ' ' << keys);
    recv_ok_reply_();
  }

  client::int_type client::sunion(const client::string_vector & keys,
                                  client::string_set & out)
  {
    send_(makecmd("SUNION") << keys);
    return recv_multi_bulk_reply_(out);
  }

  void client::sunionstore(const client::string_type & dstkey, 
                           const client::string_vector & keys)
  {
    send_(makecmd("SUNIONSTORE") << dstkey << ' ' << keys);
    recv_ok_reply_();
  }

  client::int_type client::smembers(const client::string_type & key, 
                                    client::string_set & out)
  {
    send_(makecmd("SMEMBERS") << key);
    return recv_multi_bulk_reply_(out);
  }

  void client::select(client::int_type dbindex)
  {
    send_(makecmd("SELECT") << dbindex);
    recv_ok_reply_();
  }

  void client::move(const client::string_type & key, 
                    client::int_type dbindex)
  {
    send_(makecmd("MOVE") << key << ' ' << dbindex);
    recv_int_ok_reply_();
  }

  void client::flushdb()
  {
    send_(makecmd("FLUSHDB", true));
    recv_ok_reply_();
  }

  void client::flushall()
  {
    send_(makecmd("FLUSHALL", true));
    recv_ok_reply_();
  }

  client::int_type client::sort(const client::string_type & key, 
                                client::string_vector & out,
                                client::sort_order order,
                                bool lexicographically)
  {
    send_(makecmd("SORT") << key 
          << (order == sort_order_ascending ? " ASC" : " DESC")
          << (lexicographically ? " ALPHA" : ""));

    return recv_multi_bulk_reply_(out);
  }

  client::int_type client::sort(const client::string_type & key, 
                                client::string_vector & out,
                                client::int_type limit_start, 
                                client::int_type limit_end, 
                                client::sort_order order,
                                bool lexicographically)
  {
    send_(makecmd("SORT") << key 
          << " LIMIT " << limit_start << ' ' << limit_end 
          << (order == sort_order_ascending ? " ASC" : " DESC")
          << (lexicographically ? " ALPHA" : ""));

    return recv_multi_bulk_reply_(out);
  }

  client::int_type client::sort(const client::string_type & key, 
                                client::string_vector & out,
                                const client::string_type & by_pattern, 
                                client::int_type limit_start, 
                                client::int_type limit_end, 
                                const client::string_vector & get_patterns, 
                                client::sort_order order,
                                bool lexicographically)
  {
    makecmd m("SORT");

    m << key 
      << " BY "    << by_pattern
      << " LIMIT " << limit_start << ' ' << limit_end;

    client::string_vector::const_iterator it = get_patterns.begin();
    for ( ; it != get_patterns.end(); ++it) 
      m << " GET " << *it;

    m << (order == sort_order_ascending ? " ASC" : " DESC")
      << (lexicographically ? " ALPHA" : "");

    send_(m);

    return recv_multi_bulk_reply_(out);
  }

  void client::save()
  {
    send_(makecmd("SAVE", true));
    recv_ok_reply_();
  }

  void client::bgsave()
  {
    send_(makecmd("BGSAVE", true));
    recv_ok_reply_();
  }

  time_t client::lastsave()
  {
    send_(makecmd("LASTSAVE", true));
    return recv_int_reply_();
  }

  void client::shutdown()
  {
    send_(makecmd("SHUTDOWN", true));

    // we expected to get a connection_error as redis closes the connection on shutdown command.

    try
    {
      recv_ok_reply_();
    }
    catch (connection_error & e)
    {
    }
  }

  void client::info(server_info & out)
  {
    send_(makecmd("INFO", true));
    string response = recv_bulk_reply_();

    if (response.empty())
      throw protocol_error("empty");

    string_vector lines;
    split_lines(response, lines);
    if (lines.empty())
      throw protocol_error("empty line for info");

    for (string_vector::const_iterator it = lines.begin();
         it != lines.end(); ++it)
    {
      const string & line = *it;
      string_vector line_parts;
      split(line, ':', line_parts);
      if (line_parts.size() != 2)
        throw protocol_error("unexpected line format for info");

      const string & key = line_parts[0];
      const string & val = line_parts[1];

      if (key == server_info_key_version)
        out.version = val;
      else if (key == server_info_key_bgsave_in_progress)
        out.bgsave_in_progress = unsigned_number_from_string(val) == 1;
      else if (key == server_info_key_connected_clients)
        out.connected_clients = unsigned_number_from_string(val);
      else if (key == server_info_key_connected_slaves)
        out.connected_slaves = unsigned_number_from_string(val);
      else if (key == server_info_key_used_memory)
        out.used_memory = unsigned_number_from_string(val);
      else if (key == server_info_key_changes_since_last_save)
        out.changes_since_last_save = unsigned_number_from_string(val);
      else if (key == server_info_key_last_save_time)
        out.last_save_time = unsigned_number_from_string(val);
      else if (key == server_info_key_total_connections_received)
        out.total_connections_received = unsigned_number_from_string(val);
      else if (key == server_info_key_total_commands_processed)
        out.total_commands_processed = unsigned_number_from_string(val);
      else if (key == server_info_key_uptime_in_seconds)
        out.uptime_in_seconds = unsigned_number_from_string(val);
      else if (key == server_info_key_uptime_in_days)
        out.uptime_in_days = unsigned_number_from_string(val);
      else
        throw protocol_error(string("unexpected info key '") + key + "'");
    }
  }

  // 
  // Private methods
  //

  void client::send_(const string & msg)
  {
#ifndef NDEBUG
    output_proto_debug(msg, false);
#endif

    if (anetWrite(socket_, const_cast<char *>(msg.data()), msg.size()) == -1)
      throw connection_error(strerror(errno));
  }

  string client::recv_single_line_reply_()
  {
    string line = read_line(socket_);

#ifndef NDEBUG
    output_proto_debug(line);
#endif

    if (line.empty())
      throw protocol_error("empty single line reply");

    if (line.find(prefix_status_reply_error) == 0) 
    {
      string error_msg = line.substr(prefix_status_reply_error.length());
      if (error_msg.empty()) 
        error_msg = "unknown error";
      throw protocol_error(error_msg);
    }

    if (line[0] != prefix_status_reply_value)
      throw protocol_error("unexpected prefix for status reply");

    return line.substr(1);
  }

  void client::recv_ok_reply_() 
  {
    if (recv_single_line_reply_() != status_reply_ok) 
      throw protocol_error("expected OK response");
  }

  client::int_type client::recv_bulk_reply_(char prefix)
  {
    string line = read_line(socket_);

#ifndef NDEBUG
    output_proto_debug(line);
#endif

    if (line[0] != prefix)
      throw protocol_error("unexpected prefix for bulk reply");

    return number_from_string(line.substr(1));
  }

  string client::recv_bulk_reply_() 
  {
    int_type length = recv_bulk_reply_(prefix_single_bulk_reply);

    if (length == -1)
      return client::missing_value;

    int_type real_length = length + 2;    // CRLF

    string data = read_n(socket_, real_length);

#ifndef NDEBUG
    output_proto_debug(data.substr(0, data.length()-2));
#endif

    if (data.empty())
      throw protocol_error("invalid bulk reply data; empty");

    if (data.length() != static_cast<string::size_type>(real_length))
      throw protocol_error("invalid bulk reply data; data of unexpected length");

    data.erase(data.size() - 2);

    return data;
  }

  client::int_type client::recv_multi_bulk_reply_(string_vector & out)
  {
    int_type length = recv_bulk_reply_(prefix_multi_bulk_reply);

    if (length == -1)
      throw key_error("no such key");

    for (int_type i = 0; i < length; ++i)
      out.push_back(recv_bulk_reply_());

    return length;
  }

  client::int_type client::recv_multi_bulk_reply_(string_set & out)
  {
    int_type length = recv_bulk_reply_(prefix_multi_bulk_reply);

    if (length == -1)
      throw key_error("no such key");

    for (int_type i = 0; i < length; ++i) 
      out.insert(recv_bulk_reply_());

    return length;
  }

  client::int_type client::recv_int_reply_()
  {
    string line = read_line(socket_);

#ifndef NDEBUG
    output_proto_debug(line);
#endif

    if (line.empty())
      throw protocol_error("invalid integer reply; empty");

    if (line[0] != prefix_int_reply)
      throw protocol_error("unexpected prefix for integer reply");

    return number_from_string(line.substr(1));
  }

  void client::recv_int_ok_reply_()
  {
    if (recv_int_reply_() != 1)
      throw protocol_error("expecting int reply of 1");
  }
}
