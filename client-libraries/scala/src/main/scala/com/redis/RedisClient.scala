package com.redis

import com.redis.operations._

/**
 * Redis client
 *
 */

class Redis(val host: String, val port: Int) extends Operations with ListOperations with SetOperations with NodeOperations with KeySpaceOperations with SortOperations {
  
  // auxiliary constructor
  def this() = this("localhost", 6379)
  
  // Points to the connection to a server instance
  val connection = Connection(host, port)
  var db: Int = 0
  
  // Connect and Disconnect to the Redis server
  def connect = connection.connect
  def disconnect = connection.disconnect
  def connected: Boolean = connection.connected
  
  // Establish the connection to the server instance on initialize
  connect
  
  // Get Redis Client connection.
  def getConnection(key: String) = getConnection
  def getConnection = connection
  
  // Outputs a formatted representation of the Redis server.
  override def toString = connection.host+":"+connection.port+" <connected:"+connection.connected+">"
}
