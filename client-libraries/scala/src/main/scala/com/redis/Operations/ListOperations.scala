package com.redis.operations

/**
 * Redis list operations
 *
 */

trait ListOperations{
  
  def getConnection(key: String): Connection
  
  // add the string value to the head (LPUSH) or tail (RPUSH) of the list stored at key.
  // If the key does not exist an empty list is created just before the append operation. If the key exists but is not a List an error is returned.
  // LPUSH
  def pushHead(key: String, value: String): Boolean = {
    val connection = getConnection(key)
    connection.write("LPUSH "+key+" "+value.length+"\r\n"+value+"\r\n")
    connection.readBoolean
  }
  
  // RPUSH
  def pushTail(key: String, value: String): Boolean = {
    val connection = getConnection(key)
    connection.write("RPUSH "+key+" "+value.length+"\r\n"+value+"\r\n")
    connection.readBoolean
  }
  
  // LPOP
  // atomically return and remove the first (LPOP) or last (RPOP) element of the list
  def popHead(key: String): String = {
    val connection = getConnection(key)
    connection.write("LPOP "+key+"\r\n")
    connection.readString
  }
  
  // RPOP
  // atomically return and remove the first (LPOP) or last (RPOP) element of the list
  def popTail(key: String): String = {
    val connection = getConnection(key)
    connection.write("RPOP "+key+"\r\n")
    connection.readString
  }
  
  // LINDEX
  // return the especified element of the list stored at the specified key. 0 is the first element, 1 the second and so on.
  // Negative indexes are supported, for example -1 is the last element, -2 the penultimate and so on.
  def listIndex(key: String, index: Int): String = {
    val connection = getConnection(key)
    connection.write("LINDEX "+key+" "+index+"\r\n")
    connection.readString
  }
  
  // LSET
  // set the list element at index with the new value. Out of range indexes will generate an error
  def listSet(key: String, index: Int, value: String): Boolean = {
    val connection = getConnection(key)
    connection.write("LSET "+key+" "+index+" "+value.length+"\r\n"+value+"\r\n")
    connection.readBoolean
  }
  
  // LLEN
  // return the length of the list stored at the specified key.
  // If the key does not exist zero is returned (the same behaviour as for empty lists). If the value stored at key is not a list an error is returned.
  def listLength(key: String): Int = {
    val connection = getConnection(key)
    connection.write("LLEN "+key+"\r\n")
    connection.readInt
  }
  
  // LRANGE
  // return the specified elements of the list stored at the specified key.
  // Start and end are zero-based indexes. 0 is the first element of the list (the list head), 1 the next element and so on.
  def listRange(key: String, start: Int, end: Int): List[String] = {
    val connection = getConnection(key)
    connection.write("LRANGE "+key+" "+start+" "+end+"\r\n")
    connection.readList
  }
  
  // LTRIM
  // Trim an existing list so that it will contain only the specified range of elements specified.
  def listTrim(key: String, start: Int, end: Int): Boolean = {
    val connection = getConnection(key)
    connection.write("LTRIM "+key+" "+start+" "+end+"\r\n")
    connection.readBoolean
  }
  
  // LREM
  // Remove the first count occurrences of the value element from the list.
  def listRem(key: String, count: Int, value: String): Boolean = {
    val connection = getConnection(key)
    connection.write("LREM "+key+" "+count+" "+value.length+"\r\n"+value+"\r\n")
    connection.readBoolean
  }
}
