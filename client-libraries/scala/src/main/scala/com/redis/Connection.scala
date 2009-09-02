package com.redis

/**
 * Redis client Connection
 *
 */

case class Connection(val host: String, val port: Int) extends SocketOperations
