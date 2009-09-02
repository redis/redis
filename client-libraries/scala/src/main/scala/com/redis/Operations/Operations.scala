package com.redis.operations

/**
 * Redis operations
 *
 */

trait Operations{
  
  def getConnection(key: String): Connection
  
  // SET (key, value)
  // SET (key, value, expiry)
  // sets the key with the specified value, and with an optional expiry.
  def set(key: String, value: String) = setKey(key, value)
  def set(key: String, value: String, expiry: Int) = { setKey(key, value) && expire(key, expiry) }
  
  // SET KEY (key, value)
  // sets the key with the specified value.
  def setKey(key: String, value: String): Boolean = {
    val connection = getConnection(key)
    connection.write("SET "+key+" "+value.length+"\r\n"+value+"\r\n")
    connection.readBoolean
  }
  
  // EXPIRE (key, expiry)
  // sets the expire time (in sec.) for the specified key.
  def expire(key: String, expiry: Int): Boolean = {
    val connection = getConnection(key)
    connection.write("EXPIRE "+key+" "+expiry+"\r\n")
    connection.readBoolean
  }
  
  // GET (key)
  // gets the value for the specified key.
  def get(key: String): String = {
    val connection = getConnection(key)
    val a = connection.write("GET "+key+"\r\n")
    connection.readResponse match {
      case r: String => r.toString
      case _ => null
    }
  }
  
  // GETSET (key, value)
  // is an atomic set this value and return the old value command.
  def getSet(key: String, value: String): String = {
    val connection = getConnection(key)
    val a = connection.write("GETSET "+key+" "+value.length+"\r\n"+value+"\r\n")
    connection.readResponse match {
      case r: String => r.toString
      case _ => null
    }
  }
  
  // SETNX (key, value)
  // sets the value for the specified key, only if the key is not there.
  def setUnlessExists(key: String, value: String): Boolean = {
    val connection = getConnection(key)
    connection.write("SETNX "+key+" "+value.length+"\r\n"+value+"\r\n")
    connection.readBoolean
  }
  
  // DELETE (key)
  // deletes the specified key.
  def delete(key: String): Boolean = {
    val connection = getConnection(key)
    connection.write("DEL "+key+"\r\n")
    connection.readBoolean
  }
  
  // INCR (key)
  // INCR (key, increment)
  // increments the specified key, optional the increment value.
  def incr(x: Any): Int = x match {
    case (key: String, increment: Int) => incrBy(key, increment)
    case (key: String) => incrOne(key)
    case _ => 0
  }
  def incrBy(key: String, increment: Int): Int = {
    val connection = getConnection(key)
    connection.write("INCRBY "+key+" "+increment+"\r\n")
    connection.readInt
  }
  def incrOne(key: String): Int = {
    val connection = getConnection(key)
    connection.write("INCR "+key+"\r\n")
    connection.readInt
  }
  
  // DECR (key)
  // DECRBY (key, decrement)
  // decrements the specified key, optional the decrement value.
  def decr(key: String, decrement: Int) = decrBy(key, decrement)
  def decr(key: String) = decrOne(key)
  
  def decrBy(key: String, decrement: Int): Int = {
    val connection = getConnection(key)
    connection.write("DECRBY "+key+" "+decrement+"\r\n")
    connection.readInt
  }
  def decrOne(key: String): Int = {
    val connection = getConnection(key)
    connection.write("DECR "+key+"\r\n")
    connection.readInt
  }
  
  // EXISTS (key)
  // test if the specified key exists.
  def exists(key: String): Boolean = {
    val connection = getConnection(key)
    connection.write("EXISTS "+key+"\r\n")
    connection.readBoolean
  }
  
  // TYPE (key)
  // return the type of the value stored at key in form of a string.
  def getType(key: String): Any = {
    val connection = getConnection(key)
    connection.write("TYPE "+key+"\r\n")
    connection.readResponse
  }
  
}