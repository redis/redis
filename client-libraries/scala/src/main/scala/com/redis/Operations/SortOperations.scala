package com.redis.operations

/**
 * Redis sort operations
 *
 */

trait SortOperations{
  
  def getConnection(key: String): Connection
  
  // SORT
  // Sort a Set or a List accordingly to the specified parameters.
  def sort(args: Any): List[String] = args match {
    case (key: String, command: String) => doSort(key, command)
    case (key: String) => doSort(key, "")
  }
  
  def doSort(key: String, command: String): List[String] = {
    val connection = getConnection(key)
    if(command != "") {
      connection.write("SORT "+key+" "+command+"\r\n")
    } else {
      connection.write("SORT "+key+"\r\n")
    }
    connection.readList
  }
}