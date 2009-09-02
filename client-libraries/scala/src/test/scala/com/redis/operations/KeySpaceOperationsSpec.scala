import org.specs._
import com.redis._

import org.specs.mock.Mockito
import org.mockito.Mock._
import org.mockito.Mockito._
import org.mockito.Mockito.doNothing

object KeySpaceOperationsSpec extends Specification with Mockito {

  "Redis Client Key Operations" should {
    var client: RedisTestClient = null
    var connection: Connection = null
    
    doBefore{
      connection = mock[Connection]
      client = new RedisTestClient(connection)
    }
    
    "return all keys matching" in {
      connection.readResponse returns "akey anotherkey adiffkey"
      client.keys("a*")
      connection.write("KEYS a*\r\n") was called
    }
    
    "return a random key" in {
      connection.readResponse returns "+somerandonkey"
      client.randomKey mustEqual "somerandonkey"
      connection.write("RANDOMKEY\r\n") was called
    }
    
    "remame a key" in {
      connection.readBoolean returns true
      client.rename("a", "b") must beTrue
      connection.write("RENAME a b\r\n") was called
    }
    
    "rename a key only if destintation doesn't exist" in {
      connection.readBoolean returns false
      client.renamenx("a", "b") must beFalse
      connection.write("RENAMENX a b\r\n") was called
    }
    
    "tell the size of the db, # of keys" in {
      connection.readInt returns 4
      client.dbSize mustEqual 4
      connection.write("DBSIZE\r\n") was called
    }
  }
}