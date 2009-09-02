package com.redis

/**
 * Socket operations
 *
 */

import java.io._
import java.net.Socket

trait SocketOperations {
  
  // Response codes from the Redis server
  // they tell you what's coming next from the server.
  val ERR: String    = "-"
  val OK: String     = "+OK"
  val SINGLE: String = "+"
  val BULK: String   = "$"
  val MULTI: String  = "*"
  val INT:String     = ":"
  
  val host: String
  val port: Int
  
  // File descriptors.
  var socket: Socket = null
  var out: OutputStream = null
  var in: BufferedReader = null
  
  def getOutputStream: OutputStream = out
  def getInputStream: BufferedReader = in
  def getSocket: Socket = socket

  def connected = { getSocket != null }
  def reconnect = { disconnect && connect; }
  
  // Connects the socket, and sets the input and output streams.
  def connect: Boolean = {
    try {
      socket = new Socket(host, port)
      out = getSocket.getOutputStream
      in = new BufferedReader(new InputStreamReader(getSocket.getInputStream));
      true
    } catch {
      case _ => clear_fd; false;
    }
  }
  
  // Disconnects the socket.
  def disconnect: Boolean = {
    try {
      socket.close
      out.close
      in.close
      clear_fd
      true
    } catch {
      case _ => false
    }
  }
  
  def clear_fd = {
    socket = null
    out = null
    in = null
  }
  
  // Reads the server responses as Scala types.
  def readString: String = readResponse.toString                 // Reads the server response as an Int
  def readInt: Int = Integer.parseInt(readResponse.toString)     // Reads the server response as an Int
  def readList: List[String] = listReply(readResponse.toString)  // Reads the server response as a List
  def readSet: Set[String]   = setReply(readResponse.toString)   // Reads the server response as a String
  def readBoolean: Boolean = readResponse match {
    case 1  => true
    case OK => true
    case _  => false
  }
  
  // Read from Input Stream.
  def readline: String = {
    try {
      getInputStream.readLine()
    } catch {
      case _ => ERR;
    }
  }
  
  // Gets the type of response the server is going to send.
  def readtype = {
    val res = readline
    if(res !=null){
      (res(0).toString(), res)
    }else{
      ("-", "")
    }
  }
  
  // Reads the response from the server based on the response code.
  def readResponse = {
    
    val responseType = readtype
    try{
       responseType._1 match {
        case ERR     => reconnect; // RECONNECT
        case SINGLE  => lineReply(responseType._2)
        case BULK    => bulkReply(responseType._2)
        case MULTI   => responseType._2
        case INT     => integerReply(responseType._2)
        case _       => reconnect; // RECONNECT
       }
    }catch{
      case e: Exception => false
    }
  }
  
  def integerReply(response: String): Int = Integer.parseInt(response.split(":")(1).toString)
  
  def lineReply(response: String): String = response
  
  def listReply(response: String): List[String] = {
      val total = Integer.parseInt(response.split('*')(1))
      var list: List[String] = List()
      for(i <- 1 to total){
        list = (list ::: List(bulkReply(readtype._2)))
      }
    list
  }
  
  def bulkReply(response: String) = {
    if(response(1).toString() != ERR){
      var length: Int = Integer.parseInt(response.split('$')(1).split("\r\n")(0))
      var line, res: String = ""
      while(length >= 0){
        line = readline
        length -= (line.length+2)
        res += line
        if(length > 0) res += "\r\n"
      }
      res
    }else{ null }
  }
  
  def setReply(response: String): Set[String] = {
      val total = Integer.parseInt(response.split('*')(1))
      var set: Set[String] = Set()
      for(i <- 1 to total){
        set += bulkReply(readtype._2)
      }
    set
  }
  
  // Wraper for the socket write operation.
  def write_to_socket(data: String)(op: OutputStream => Unit) = op(getOutputStream)
  
  // Writes data to a socket using the specified block.
  def write(data: String) = {
    if(!connected) connect;
    write_to_socket(data){
      getSocket => 
        try { 
          getSocket.write(data.getBytes)
        } catch {
          case _ => reconnect;
        }
    }
  }
}