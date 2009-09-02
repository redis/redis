package com.redis.operations

/**
 * Redis set operations
 *
 */

trait SetOperations{
  
  def getConnection(key: String): Connection
  
  // SADD
  // Add the specified member to the set value stored at key.
  def setAdd(key: String, value: String): Boolean = {
    val connection = getConnection(key)
    connection.write("SADD "+key+" "+value.length+"\r\n"+value+"\r\n")
    connection.readBoolean
  }
  
  // SREM
  // Remove the specified member from the set value stored at key.
  def setDelete(key: String, value: String): Boolean = {
    val connection = getConnection(key)
    connection.write("SREM "+key+" "+value.length+"\r\n"+value+"\r\n")
    connection.readBoolean
  }
  
  // SCARD
  // Return the number of elements (the cardinality) of the Set at key.
  def setCount(key: String): Int = {
    val connection = getConnection(key)
    connection.write("SCARD "+key+"\r\n")
    connection.readInt
  }
  
  // SMEMBERS
  // Return all the members of the Set value at key.
  def setMembers(key: String): Set[String] = {
    val connection = getConnection(key)
    connection.write("SMEMBERS "+key+"\r\n")
    connection.readSet
  }
  
  // SPOP
  // Remove and return (pop) a random element from the Set value at key.
  def setPop(key: String): String = {
    val connection = getConnection(key)
    connection.write("SPOP "+key+"\r\n")
    connection.readString
  }
  
  // SMOVE
  // Move the specified member from one Set to another atomically.
  def setMove(sourceKey: String, destKey: String, value: String): Boolean = {
    val connection = getConnection(sourceKey)
    connection.write("SMOVE "+sourceKey+" "+destKey+" "+value+"\r\n")
    connection.readBoolean
  }
  
  // SISMEMBER
  // Test if the specified value is a member of the Set at key.
  def setMemberExists(key: String, value: String): Boolean = {
    val connection = getConnection(key)
    connection.write("SISMEMBER "+key+" "+value.length+"\r\n"+value+"\r\n")
    connection.readBoolean
  }
  
  // SINTER
  // Return the intersection between the Sets stored at key1, key2, ..., keyN.
  def setIntersect(keys: String*): Set[String] = {
    val connection = getConnection(keys(0))
    connection.write("SINTER "+keys.mkString(" ")+"\r\n")
    connection.readSet
  }
  
  // SINTERSTORE
  // Compute the intersection between the Sets stored at key1, key2, ..., keyN, and store the resulting Set at dstkey.
  def setInterStore(key: String, keys: String*): Boolean = {
    val connection = getConnection(key)
    connection.write("SINTERSTORE "+key+" "+keys.mkString(" ")+"\r\n")
    connection.readBoolean
  }
  
  // SDIFF
  // Return the difference between the Set stored at key1 and all the Sets key2, ..., keyN.
  def setDiff(keys: String*): Set[String] = {
    val connection = getConnection(keys(0))
    connection.write("SDIFF "+keys.mkString(" ")+"\r\n")
    connection.readSet
  }
  
  // SDIFFSTORE
  // Compute the difference between the Set key1 and all the Sets key2, ..., keyN, and store the resulting Set at dstkey.
  def setDiffStore(key: String, keys: String*): Boolean = {
    val connection = getConnection(key)
    connection.write("SDIFFSTORE "+key+" "+keys.mkString(" ")+"\r\n")
    connection.readBoolean
  }
  
  // SUNION
  // Return the union between the Sets stored at key1, key2, ..., keyN.
  def setUnion(keys: String*): Set[String] = {
    val connection = getConnection(keys(0))
    connection.write("SUNION "+keys.mkString(" ")+"\r\n")
    connection.readSet
  }
  
  // SUNIONSTORE
  // Compute the union between the Sets stored at key1, key2, ..., keyN, and store the resulting Set at dstkey.
  def setUnionStore(key: String, keys: String*): Boolean = {
    val connection = getConnection(key)
    connection.write("SUNIONSTORE "+key+" "+keys.mkString(" ")+"\r\n")
    connection.readBoolean
  }
}