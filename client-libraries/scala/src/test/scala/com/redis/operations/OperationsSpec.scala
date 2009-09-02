import org.specs._
import com.redis._

import org.specs.mock.Mockito
import org.mockito.Mock._
import org.mockito.Mockito._

object OperationsSpec extends Specification with Mockito {
  
  "Redis Client Operations" should {
    
    var client: RedisTestClient = null
    var connection: Connection = null
    
    doBefore{
      connection = mock[Connection]
      client = new RedisTestClient(connection)
    }
    
    "set a key" in {
      connection.readBoolean returns true
      client.set("a", "b") mustEqual true
      connection.write("SET a 1\r\nb\r\n") was called
    }
    
    "set a key with setKey" in {
      connection.readBoolean returns true
      client.setKey("a", "b") mustEqual true
      connection.write("SET a 1\r\nb\r\n") was called
    }
    
    "set a key with expiration" in {
      connection.readBoolean returns true
      client.set("a", "b", 4) mustEqual true
      connection.write("SET a 1\r\nb\r\n") was called
      connection.write("EXPIRE a 4\r\n") was called
    }
    
    "expire a key" in {
      connection.readBoolean returns true
      client.expire("a", 4) mustEqual true
      connection.write("EXPIRE a 4\r\n") was called
    }
    
    "get a key" in {
      connection.readResponse returns "b"
      client.get("a") mustEqual "b"
      connection.write("GET a\r\n") was called
    }
    
    "get and set a key" in {
      connection.readResponse returns "old"
      client.getSet("a", "new") mustEqual "old"
      connection.write("GETSET a 3\r\nnew\r\n") was called
    }
    
    "delete a key" in {
      connection.readBoolean returns true
      client.delete("a") mustEqual true
      connection.write("DEL a\r\n") was called
    }
    
    "tell if a key exists" in {
      connection.readBoolean returns true
      client.exists("a") mustEqual true
      connection.write("EXISTS a\r\n") was called
    }
    
    "tell if a key exists" in {
      connection.readBoolean returns true
      client.exists("a") mustEqual true
      connection.write("EXISTS a\r\n") was called
    }
    
    "increment a value" in {
      connection.readInt returns 1
      client.incr("a") mustEqual 1
      connection.write("INCR a\r\n") was called
    }
    
    "increment a value by N" in {
      connection.readInt returns 27
      client.incr("a", 23) mustEqual 27
      connection.write("INCRBY a 23\r\n") was called
    }
    
    "decrement a value" in {
      connection.readInt returns 0
      client.decr("a") mustEqual 0
      connection.write("DECR a\r\n") was called
    }
    
    "decrement a value by N" in {
      connection.readInt returns 25
      client.decr("a", 2) mustEqual 25
      connection.write("DECRBY a 2\r\n") was called
    }
    
    "return type of key" in {
      connection.readResponse returns "String"
      client.getType("a") mustEqual "String"
      connection.write("TYPE a\r\n") was called
    }
  }
}
