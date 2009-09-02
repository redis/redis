package com.redis

import com.redis.operations._

/**
 * Redis cluster
 *
 */

import scala.collection.mutable.ArrayBuffer

class RedisCluster(val hosts: String*) extends Operations with ListOperations with SetOperations with HashRing with SortOperations {
  
  // Get Redis Client connection inside the HashRing.
  def getConnection(key: String) = {
    getNode(key).connection
  }
  
  // Default value used on MemCache client.
  private val NUMBER_OF_REPLICAS = 160
  val replicas = NUMBER_OF_REPLICAS
  
  // Outputs a formatted representation of the Redis server.
  override def toString = cluster.mkString(", ")
  
  // Connect the client and add it to the cluster.
  def connectClient(host: String): Boolean = {
   val h = host.split(":")(0)
   val p = host.split(":")(1)
   val client = new Redis(h.toString, p.toString.toInt)
   addNode(client)
   client.connected
  }
  
  // Connect all clients in the cluster.
  def connect = cluster.map(c => c.connect)
  
  // Initialize cluster.
  def initialize_cluster = hosts.map(connectClient(_))
  
  initialize_cluster
}
