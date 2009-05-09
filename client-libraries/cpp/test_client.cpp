#include "redisclient.h"

#include <iostream>

using namespace std;

#define ASSERT_EQUAL(x,y) assert_equal(x, y, __LINE__)
#define ASSERT_NOT_EQUAL(x,y) assert_not_equal(x, y, __LINE__)
#define ASSERT_GT(x,y) assert_gt(x, y, __LINE__)

template <typename T>
void assert_equal(const T & actual, const T & expected, int lineno)
{
#ifndef NDEBUG
  cerr << "assert_equal('" << expected << "', '" << actual << "')" << endl;
#endif

  if (expected != actual)
  {
    cerr << "expected '" << expected << "' got '" << actual << "'" << endl
         << "failing test called from line " << lineno << endl;
    
    exit(1);
  }

#ifndef NDEBUG
  cerr << "... OK" << endl;
#endif
}

template <typename T>
void assert_not_equal(const T & a, const T & b, int lineno)
{
  if (a == b)
  {
    cerr << "expected inequality" << endl
         << "failing test called from line " << lineno << endl;
    
    exit(1);
  }
}

template <typename T>
void assert_gt(const T & a, const T & b, int lineno)
{
#ifndef NDEBUG
  cerr << "assert_gt('" << a << "', '" << b << "')" << endl;
#endif

  if (a <= b)
  {
    cerr << "expected '" << a << "' > '" << b << "'" << endl
         << "failing test called from line " << lineno << endl;
    
    exit(1);
  }

#ifndef NDEBUG
  cerr << "... OK" << endl;
#endif
}

void test(const string & name)
{
#ifndef NDEBUG
  cerr << "------------------------------" << endl
       << "starting test: "                << name << endl;
#endif
}

int main(int argc, char ** argv)
{
  try 
  {
    redis::client c;

    // Test on high number databases

    c.select(14);
    c.flushdb();

    c.select(15);
    c.flushdb();

    string foo("foo"), bar("bar"), baz("baz"), buz("buz"), goo("goo");

    test("auth");
    {
      // TODO ... needs a conf for redis-server
    }

    test("info");
    {
      // doesn't throw? then, has valid numbers and known info-keys.
      redis::server_info info;
      c.info(info);
    }

    test("set, get");
    {
      c.set(foo, bar);
      ASSERT_EQUAL(c.get(foo), bar);
    }

    test("getset");
    {
      ASSERT_EQUAL(c.getset(foo, baz), bar);
      ASSERT_EQUAL(c.get(foo), baz);
    }

    test("mget");
    {
      string x_val("hello"), y_val("world");
      c.set("x", x_val);
      c.set("y", y_val);
      redis::client::string_vector keys;
      keys.push_back("x");
      keys.push_back("y");
      redis::client::string_vector vals;
      c.mget(keys, vals);
      ASSERT_EQUAL(vals.size(), size_t(2));
      ASSERT_EQUAL(vals[0], x_val);
      ASSERT_EQUAL(vals[1], y_val);
    }

    test("setnx");
    {
      ASSERT_EQUAL(c.setnx(foo, bar), false);
      ASSERT_EQUAL(c.setnx(buz, baz), true);
      ASSERT_EQUAL(c.get(buz), baz);
    }

    test("incr");
    {
      ASSERT_EQUAL(c.incr("goo"), 1L);test("nonexistent (0) -> 1");
      ASSERT_EQUAL(c.incr("goo"), 2L);test("1->2");
    }

    test("decr");
    {
      ASSERT_EQUAL(c.decr("goo"), 1L);test("2->1");
      ASSERT_EQUAL(c.decr("goo"), 0L);test("1->0");
    }

    test("incrby");
    {
      ASSERT_EQUAL(c.incrby("goo", 3), 3L);test("0->3");
      ASSERT_EQUAL(c.incrby("goo", 2), 5L);test("3->5");
    }

    test("exists");
    {
      ASSERT_EQUAL(c.exists("goo"), true);
    }

    test("del");
    {
      c.del("goo");
      ASSERT_EQUAL(c.exists("goo"), false);
    }

    test("type (basic)");
    {
      ASSERT_EQUAL(c.type(goo), redis::client::datatype_none);test("we deleted it");
      c.set(goo, "redis");
      ASSERT_EQUAL(c.type(goo), redis::client::datatype_string);
    }

    test("keys");
    {
      redis::client::string_vector keys;
      ASSERT_EQUAL(c.keys("*oo", keys), 2L);
      ASSERT_EQUAL(keys.size(), 2UL);
      ASSERT_EQUAL(keys[0], foo);
      ASSERT_EQUAL(keys[1], goo);
    }

    test("randomkey");
    {
      ASSERT_GT(c.randomkey().size(), 0UL);
    }

    test("rename");
    {
      ASSERT_EQUAL(c.exists("foo"), true);
      ASSERT_EQUAL(c.exists("doo"), false);
      c.rename("foo", "doo");
      ASSERT_EQUAL(c.exists("foo"), false);
      ASSERT_EQUAL(c.exists("doo"), true);
    }

    test("renamenx");
    {
      ASSERT_EQUAL(c.exists("doo"), true);
      ASSERT_EQUAL(c.exists("foo"), false);
      ASSERT_EQUAL(c.renamenx("doo", "foo"), true);
      ASSERT_EQUAL(c.exists("doo"), false);
      ASSERT_EQUAL(c.exists("foo"), true);
      ASSERT_EQUAL(c.renamenx("goo", "foo"), false);
      ASSERT_EQUAL(c.exists("foo"), true);
      ASSERT_EQUAL(c.exists("goo"), true);
    }

    test("dbsize");
    {
      ASSERT_GT(c.dbsize(), 0L);
    }

    test("expire");
    {
      c.expire("goo", 1);
#ifndef NDEBUG
      cerr << "please wait a few seconds.." << endl;
#endif
      sleep(2);
      ASSERT_EQUAL(c.exists("goo"), false);
    }

    test("rpush");
    {
      ASSERT_EQUAL(c.exists("list1"), false);
      c.rpush("list1", "val1");
      ASSERT_EQUAL(c.llen("list1"), 1L);
      ASSERT_EQUAL(c.type("list1"), redis::client::datatype_list);
      c.rpush("list1", "val2");
      ASSERT_EQUAL(c.llen("list1"), 2L);
      ASSERT_EQUAL(c.lindex("list1", 0), string("val1"));
      ASSERT_EQUAL(c.lindex("list1", 1), string("val2"));
    }

    test("lpush");
    {
      c.del("list1");
      ASSERT_EQUAL(c.exists("list1"), false);
      c.lpush("list1", "val1");
      ASSERT_EQUAL(c.type("list1"), redis::client::datatype_list);
      ASSERT_EQUAL(c.llen("list1"), 1L);
      c.lpush("list1", "val2");
      ASSERT_EQUAL(c.llen("list1"), 2L);
      ASSERT_EQUAL(c.lindex("list1", 0), string("val2"));
      ASSERT_EQUAL(c.lindex("list1", 1), string("val1"));
    }

    test("llen");
    {
      c.del("list1");
      ASSERT_EQUAL(c.exists("list1"), false);
      ASSERT_EQUAL(c.llen("list1"), 0L);
      c.lpush("list1", "x");
      ASSERT_EQUAL(c.llen("list1"), 1L);
      c.lpush("list1", "y");
      ASSERT_EQUAL(c.llen("list1"), 2L);
    }

    test("lrange");
    {
      ASSERT_EQUAL(c.exists("list1"), true);
      ASSERT_EQUAL(c.llen("list1"), 2L);
      redis::client::string_vector vals;
      ASSERT_EQUAL(c.lrange("list1", 0, -1, vals), 2L);
      ASSERT_EQUAL(vals.size(), 2UL);
      ASSERT_EQUAL(vals[0], string("y"));
      ASSERT_EQUAL(vals[1], string("x"));
    }

    test("lrange with subset of full list");
    {
      ASSERT_EQUAL(c.exists("list1"), true);
      ASSERT_EQUAL(c.llen("list1"), 2L);
      redis::client::string_vector vals;
      ASSERT_EQUAL(c.lrange("list1", 0, 1, vals), 2L); // inclusive, so entire list
      ASSERT_EQUAL(vals.size(), 2UL);
      ASSERT_EQUAL(vals[0], string("y"));
      ASSERT_EQUAL(vals[1], string("x"));

      redis::client::string_vector vals2;
      ASSERT_EQUAL(c.lrange("list1", 0, 0, vals2), 1L); // inclusive, so first item
      ASSERT_EQUAL(vals2.size(), 1UL);
      ASSERT_EQUAL(vals2[0], string("y"));

      redis::client::string_vector vals3;
      ASSERT_EQUAL(c.lrange("list1", -1, -1, vals3), 1L); // inclusive, so first item
      ASSERT_EQUAL(vals3.size(), 1UL);
      ASSERT_EQUAL(vals3[0], string("x"));
    }

    test("get_list");
    {
      ASSERT_EQUAL(c.exists("list1"), true);
      ASSERT_EQUAL(c.llen("list1"), 2L);
      redis::client::string_vector vals;
      ASSERT_EQUAL(c.get_list("list1", vals), 2L);
      ASSERT_EQUAL(vals.size(), 2UL);
      ASSERT_EQUAL(vals[0], string("y"));
      ASSERT_EQUAL(vals[1], string("x"));
    }

    test("ltrim");
    {
      ASSERT_EQUAL(c.exists("list1"), true);
      ASSERT_EQUAL(c.llen("list1"), 2L);
      c.ltrim("list1", 0, 0);
      ASSERT_EQUAL(c.exists("list1"), true);
      ASSERT_EQUAL(c.llen("list1"), 1L);
      redis::client::string_vector vals;
      ASSERT_EQUAL(c.get_list("list1", vals), 1L);
      ASSERT_EQUAL(vals[0], string("y"));
    }

    test("lindex");
    {
      ASSERT_EQUAL(c.lindex("list1", 0), string("y"));
      c.rpush("list1", "x");
      ASSERT_EQUAL(c.llen("list1"), 2L);
      ASSERT_EQUAL(c.lindex("list1", -1), string("x"));
      ASSERT_EQUAL(c.lindex("list1", 1), string("x"));
    }

    test("lset");
    {
      c.lset("list1", 1, "z");
      ASSERT_EQUAL(c.lindex("list1", 1), string("z"));
      ASSERT_EQUAL(c.llen("list1"), 2L);
    }

    test("lrem");
    {
      c.lrem("list1", 1, "z");
      ASSERT_EQUAL(c.llen("list1"), 1L);
      ASSERT_EQUAL(c.lindex("list1", 0), string("y"));

      // list1 = [ y ]
      ASSERT_EQUAL(c.lrem("list1", 0, "q"), 0L);

      c.rpush("list1", "z");
      c.rpush("list1", "z");
      c.rpush("list1", "z");
      c.rpush("list1", "a");     
      // list1 = [ y, z, z, z, a ]
      ASSERT_EQUAL(c.lrem("list1", 2, "z"), 2L);
      // list1 = [ y, z, a ]
      ASSERT_EQUAL(c.llen("list1"), 3L);
      ASSERT_EQUAL(c.lindex("list1", 0), string("y"));
      ASSERT_EQUAL(c.lindex("list1", 1), string("z"));
      ASSERT_EQUAL(c.lindex("list1", 2), string("a"));

      c.rpush("list1", "z");
      // list1 = [ y, z, a, z ]
      ASSERT_EQUAL(c.lrem("list1", -1, "z"), 1L);  // <0 => rm R to L 
      // list1 = [ y, z, a ]
      ASSERT_EQUAL(c.llen("list1"), 3L);
      ASSERT_EQUAL(c.lindex("list1", 0), string("y"));
      ASSERT_EQUAL(c.lindex("list1", 1), string("z"));
      ASSERT_EQUAL(c.lindex("list1", 2), string("a"));

      // list1 = [ y, z, a ]
      // try to remove 5 'a's but there's only 1 ... no problem.
      ASSERT_EQUAL(c.lrem("list1", 5, "a"), 1L);
      // list1 = [ y, z ]
      ASSERT_EQUAL(c.llen("list1"), 2L);
      ASSERT_EQUAL(c.lindex("list1", 0), string("y"));
      ASSERT_EQUAL(c.lindex("list1", 1), string("z"));
    }

    test("lrem_exact");
    {
      // list1 = [ y, z ]

      // try to remove 5 'z's but there's only 1 ... now it's a problem.

      bool threw = false;

      try 
      {
        c.lrem_exact("list1", 5, "z");
      }
      catch (redis::value_error & e)
      {
        threw = true;
      }

      ASSERT_EQUAL(threw, true);

      // This DOES remove the one 'z' though
      // list1 = [ y ]

      ASSERT_EQUAL(c.llen("list1"), 1L);
      ASSERT_EQUAL(c.lindex("list1", 0), string("y"));
    }

    test("lpop");
    {
      ASSERT_EQUAL(c.lpop("list1"), string("y")); 
      // list1 = []
      ASSERT_EQUAL(c.lpop("list1"), redis::client::missing_value);
    }

    test("rpop");
    {
      c.rpush("list1", "hello");
      c.rpush("list1", "world");
      ASSERT_EQUAL(c.rpop("list1"), string("world")); 
      ASSERT_EQUAL(c.rpop("list1"), string("hello")); 
      ASSERT_EQUAL(c.lpop("list1"), redis::client::missing_value);
    }

    test("sadd");
    {
      c.sadd("set1", "sval1");
      ASSERT_EQUAL(c.exists("set1"), true);
      ASSERT_EQUAL(c.type("set1"), redis::client::datatype_set);
      ASSERT_EQUAL(c.sismember("set1", "sval1"), true);
    }

    test("srem");
    {
      c.srem("set1", "sval1");
      ASSERT_EQUAL(c.exists("set1"), true);
      ASSERT_EQUAL(c.type("set1"), redis::client::datatype_set);
      ASSERT_EQUAL(c.sismember("set1", "sval1"), false);
    }

    test("smove");
    {
      c.sadd("set1", "hi");
      // set1 = { hi }
      ASSERT_EQUAL(c.exists("set2"), false);
      c.smove("set1", "set2", "hi");
      ASSERT_EQUAL(c.sismember("set1", "hi"), false);
      ASSERT_EQUAL(c.sismember("set2", "hi"), true); 
    }

    test("scard");
    {
      ASSERT_EQUAL(c.scard("set1"), 0L);
      ASSERT_EQUAL(c.scard("set2"), 1L);
    }

    test("sismember");
    {
      // see above
    }

    test("smembers");
    {
      c.sadd("set2", "bye");
      redis::client::string_set members;
      ASSERT_EQUAL(c.smembers("set2", members), 2L);
      ASSERT_EQUAL(members.size(), 2UL);
      ASSERT_NOT_EQUAL(members.find("hi"),  members.end());
      ASSERT_NOT_EQUAL(members.find("bye"), members.end());
    }

    test("sinter");
    {
      c.sadd("set3", "bye");
      c.sadd("set3", "bye2");
      redis::client::string_vector keys;
      keys.push_back("set2");
      keys.push_back("set3");
      redis::client::string_set intersection;
      ASSERT_EQUAL(c.sinter(keys, intersection), 1L);
      ASSERT_EQUAL(intersection.size(), 1UL);
      ASSERT_NOT_EQUAL(intersection.find("bye"), intersection.end());
    }

    test("sinterstore");
    {
      c.sadd("seta", "1");
      c.sadd("seta", "2");
      c.sadd("seta", "3");

      c.sadd("setb", "2");
      c.sadd("setb", "3");
      c.sadd("setb", "4");

      redis::client::string_vector keys;
      keys.push_back("seta");
      keys.push_back("setb");

      c.sinterstore("setc", keys);

      redis::client::string_set members;
      ASSERT_EQUAL(c.smembers("setc", members), 2L);
      ASSERT_EQUAL(members.size(), 2UL);
      ASSERT_NOT_EQUAL(members.find("2"), members.end());
      ASSERT_NOT_EQUAL(members.find("3"), members.end());
    }

    test("sunion");
    {
      c.sadd("setd", "1");
      c.sadd("sete", "2");
      redis::client::string_vector keys;
      keys.push_back("setd");
      keys.push_back("sete");
      redis::client::string_set a_union;
      ASSERT_EQUAL(c.sunion(keys, a_union), 2L);
      ASSERT_EQUAL(a_union.size(), 2UL);
      ASSERT_NOT_EQUAL(a_union.find("1"), a_union.end());
      ASSERT_NOT_EQUAL(a_union.find("2"), a_union.end());
    }

    test("sunionstore");
    {
      c.sadd("setf", "1");
      c.sadd("setg", "2");

      redis::client::string_vector keys;
      keys.push_back("setf");
      keys.push_back("setg");

      c.sunionstore("seth", keys);

      redis::client::string_set members;
      ASSERT_EQUAL(c.smembers("seth", members), 2L);
      ASSERT_EQUAL(members.size(), 2UL);
      ASSERT_NOT_EQUAL(members.find("1"), members.end());
      ASSERT_NOT_EQUAL(members.find("2"), members.end());
    }

    test("move");
    {
      c.select(14);
      ASSERT_EQUAL(c.exists("ttt"), false);
      c.select(15);
      c.set("ttt", "uuu");
      c.move("ttt", 14);
      c.select(14);
      ASSERT_EQUAL(c.exists("ttt"), true);
      c.select(15);
      ASSERT_EQUAL(c.exists("ttt"), false);
    }

    test("move should fail since key exists already");
    {
      c.select(14);
      c.set("ttt", "xxx");
      c.select(15);
      c.set("ttt", "uuu");

      bool threw = false;

      try 
      {
        c.move("ttt", 14);
      }
      catch (redis::protocol_error & e)
      {
        threw = true;
      }

      ASSERT_EQUAL(threw, true);

      c.select(14);
      ASSERT_EQUAL(c.exists("ttt"), true);
      c.select(15);
      ASSERT_EQUAL(c.exists("ttt"), true);
    }

    test("sort ascending");
    {
      c.sadd("sort1", "3");
      c.sadd("sort1", "2");
      c.sadd("sort1", "1");

      redis::client::string_vector sorted;
      ASSERT_EQUAL(c.sort("sort1", sorted), 3L);
      ASSERT_EQUAL(sorted.size(), 3UL);
      ASSERT_EQUAL(sorted[0], string("1"));
      ASSERT_EQUAL(sorted[1], string("2"));
      ASSERT_EQUAL(sorted[2], string("3"));
    }

    test("sort descending");
    {
      redis::client::string_vector sorted;
      ASSERT_EQUAL(c.sort("sort1", sorted, redis::client::sort_order_descending), 3L);
      ASSERT_EQUAL(sorted.size(), 3UL);
      ASSERT_EQUAL(sorted[0], string("3"));
      ASSERT_EQUAL(sorted[1], string("2"));
      ASSERT_EQUAL(sorted[2], string("1"));
    }

    test("sort with limit");
    {
      // TODO
    }

    test("sort lexicographically");
    {
      // TODO
    }

    test("sort with pattern and weights");
    {
      // TODO
    }

    test("save");
    {
      c.save();
    }

    test("bgsave");
    {
      c.bgsave();
    }

    test("lastsave");
    {
      ASSERT_GT(c.lastsave(), 0L);
    }

    test("shutdown");
    {
// You can test this if you really want to ...
//      c.shutdown();
    }
  } 
  catch (redis::redis_error & e) 
  {
    cerr << "got exception: " << string(e) << endl << "FAIL" << endl;
    return 1;
  }

  cout << endl << "testing completed successfully" << endl;
  return 0;
}
