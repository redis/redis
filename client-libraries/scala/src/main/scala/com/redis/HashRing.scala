package com.redis

/**
 * Hash Ring
 *
 */

import java.util.zip.CRC32
import scala.collection.mutable.ArrayBuffer
import scala.collection.mutable.Map

trait HashRing {
  
  val replicas: Int
  
  var sortedKeys: List[Long] = List()
  var cluster = new ArrayBuffer[Redis]
  val ring = Map[Long, Redis]()
  
  // Adds the node to the hashRing
  // including a number of replicas.
  def addNode(node: Redis) = {
    cluster += node
    (1 to replicas).foreach{ replica => 
      val key = calculateChecksum(node+":"+replica)
      ring += (key -> node)
      sortedKeys = sortedKeys ::: List(key)
    }
    sortedKeys = sortedKeys.sort(_ < _)
  }
  
  // get the node in the hash ring for this key
  def getNode(key: String) = getNodePos(key)._1
  
  def getNodePos(key: String): (Redis, Int) = {
    val crc = calculateChecksum(key)
    val idx = binarySearch(crc)
    (ring(sortedKeys(idx)), idx)
  }
  
  // TODO this should perform a Bynary search
  def binarySearch(value: Long): Int = {
    var upper = (sortedKeys.length -1)
    var lower = 0
    var idx   = 0
    var comp: Long = 0
    
    while(lower <= upper){
      idx = (lower + upper) / 2
      comp = sortedKeys(idx)
      
      if(comp == value) { return idx }
      if(comp < value)  { upper = idx -1 }
      if(comp > value)  { lower = idx +1 }
    }
    return upper
  }
  
  // Computes the CRC-32 of the given String
  def calculateChecksum(value: String): Long = {
    val checksum = new CRC32
    checksum.update(value.getBytes)
    checksum.getValue
  }
}

