package com.redis.operations

/**
 * Redis node operations
 *
 */

trait NodeOperations {
  
  val connection: Connection
  var db: Int
  
  // SAVE
  // save the DB on disk now.
  def save: Boolean = {
    connection.write("SAVE\r\n")
    connection.readBoolean
  }
  
  // BGSAVE
  // save the DB in the background.
  def bgSave: Boolean = {
    connection.write("BGSAVE\r\n")
    connection.readBoolean
  }
  
  // LASTSAVE
  // return the UNIX TIME of the last DB SAVE executed with success.
  def lastSave: Int = {
    connection.write("LASTSAVE\r\n")
    connection.readInt
  }
  
  // SHUTDOWN
  // Stop all the clients, save the DB, then quit the server.
  def shutdown: Boolean = {
    connection.write("SHUTDOWN\r\n")
    connection.readBoolean
  }
  
  // MGET (key, key, key, ...)
  // get the values of all the specified keys.
  def mget(keys: String*) = {
    connection.write("MGET "+keys.mkString(" ")+"\r\n")
    connection.readList
  }
  
  // INFO
  // the info command returns different information and statistics about the server.
  def info = {
    connection.write("INFO\r\n")
    connection.readResponse
  }
  
  // MONITOR
  // is a debugging command that outputs the whole sequence of commands received by the Redis server.
  def monitor: Boolean = {
    connection.write("MONITOR\r\n")
    connection.readBoolean
  }
  
  // SLAVEOF
  // The SLAVEOF command can change the replication settings of a slave on the fly.
  def slaveOf(options: Any): Boolean = options match {
    case (host: String, port: Int) => {
      connection.write("SLAVEOF "+host+" "+port.toString+"\r\n")
      connection.readBoolean
    }
    case _ => setAsMaster
  }
  
  def setAsMaster: Boolean = {
    connection.write("SLAVEOF NO ONE\r\n")
    connection.readBoolean
  }
  
  // SELECT (index)
  // selects the DB to connect, defaults to 0 (zero).
  def selectDb(index: Int): Boolean = {
    connection.write("SELECT "+index+"\r\n")
    connection.readBoolean match {
      case true => { db = index; true }
      case _ => false
    }
  }
  
  // FLUSHDB the DB
  // removes all the DB data.
  def flushDb: Boolean = {
    connection.write("FLUSHDB\r\n")
    connection.readBoolean
  }
  
  // FLUSHALL the DB's
  // removes data from all the DB's.
  def flushAll: Boolean = {
    connection.write("FLUSHALL\r\n")
    connection.readBoolean
  }
  
  // MOVE
  // Move the specified key from the currently selected DB to the specified destination DB.
  def move(key: String, db: Int) = {
    connection.write("MOVE "+key+" "+db.toString+"\r\n")
    connection.readBoolean
  }
  
  // QUIT
  // exits the server.
  def quit: Boolean = {
    connection.write("QUIT\r\n")
    connection.disconnect
  }
  
  // AUTH
  // auths with the server.
  def auth(secret: String): Boolean = {
    connection.write("AUTH "+secret+"\r\n")
    connection.readBoolean
  }
}
