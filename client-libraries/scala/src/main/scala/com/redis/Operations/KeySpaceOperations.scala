package com.redis.operations

/**
 * Redis key space operations
 *
 */

trait KeySpaceOperations{
  
  val connection: Connection
  var db: Int
  
  // KEYS
  // returns all the keys matching the glob-style pattern.
  def keys(pattern: String): Array[String] = {
    connection.write("KEYS "+pattern+"\r\n")
    connection.readResponse.toString.split(" ")
  }
  
  // RANDKEY
  // return a randomly selected key from the currently selected DB.
  def randomKey: String = {
    connection.write("RANDOMKEY\r\n")
    connection.readResponse.toString.split('+')(1)
  }
  
  // RENAME (oldkey, newkey)
  // atomically renames the key oldkey to newkey.
  def rename(oldkey: String, newkey: String): Boolean = {
    connection.write("RENAME "+oldkey+" "+newkey+"\r\n")
    connection.readBoolean
  }
  
  // RENAMENX (oldkey, newkey)
  // rename oldkey into newkey but fails if the destination key newkey already exists.
  def renamenx(oldkey: String, newkey: String): Boolean = {
    connection.write("RENAMENX "+oldkey+" "+newkey+"\r\n")
    connection.readBoolean
  }
  
  // DBSIZE
  // return the size of the db.
  def dbSize: Int = {
    connection.write("DBSIZE\r\n")
    connection.readInt
  }
}