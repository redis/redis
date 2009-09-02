import org.specs._
import com.redis._

import org.specs.mock.Mockito
import org.mockito.Mock._
import org.mockito.Mockito._
import org.mockito.Mockito.doNothing

object NodeOperationsSpec extends Specification with Mockito {

  "Redis Client Node Operations" should {
    var client: RedisTestClient = null
    var connection: Connection = null
    
    doBefore{
      connection = mock[Connection]
      client = new RedisTestClient(connection)
    }
    
    "save the db to disk" in {
      connection.readBoolean returns true
      client.save must beTrue
      connection.write("SAVE\r\n") was called
    }
    
    "return the last time saved data to the db" in {
      connection.readInt returns 1250421891
      client.lastSave mustEqual  1250421891
      connection.write("LASTSAVE\r\n") was called
    }
    
    "return all specified keys" in {
      connection.readList returns List[String]("hola", null, null)
      client.mget("a", "b", "c") mustEqual List[String]("hola", null, null)
      connection.write("MGET a b c\r\n") was called
    }
    
    "return server info" in {
      val sampleInfo = "res0: Any = \nredis_version:0.091\nconnected_clients:2\nconnected_slaves:0\nused_memory:3036\nchanges_since_last_save:0\nlast_save_time:1250440893\ntotal_connections_received:2\ntotal_commands_processed:0\nuptime_in_seconds:7\nuptime_in_days:0\n"
      connection.readResponse returns sampleInfo
      client.info mustEqual sampleInfo
      connection.write("INFO\r\n") was called
    }
    
    "start monitor debug on the server" in {
      connection.readBoolean returns true
      client.monitor mustEqual true
      connection.write("MONITOR\r\n") was called
    }
    
    "set a server as slave of a remote master" in {
      connection.readBoolean returns true
      client.slaveOf("localhost", 9999) mustEqual true
      connection.write("SLAVEOF localhost 9999\r\n") was called
    }
    
    "set a server as master if no params sent" in {
      connection.readBoolean returns true
      client.slaveOf() mustEqual true
      connection.write("SLAVEOF NO ONE\r\n") was called
    }
    
    "set a server as master" in {
      connection.readBoolean returns true
      client.setAsMaster mustEqual true
      connection.write("SLAVEOF NO ONE\r\n") was called
    }
    
    "select the current db" in {
      connection.readBoolean returns true
      client.selectDb(3) mustEqual true
      client.db mustEqual 3
      connection.write("SELECT 3\r\n") was called
    }
    
    "flush the db" in {
      connection.readBoolean returns true
      client.flushDb mustEqual true
      connection.write("FLUSHDB\r\n") was called
    }
    
    "flush all the dbs" in {
      connection.readBoolean returns true
      client.flushAll mustEqual true
      connection.write("FLUSHALL\r\n") was called
    }
    
    "shutdown the db" in {
      connection.readBoolean returns true
      client.shutdown mustEqual true
      connection.write("SHUTDOWN\r\n") was called
    }
    
    "move keys from one db to another" in {
      connection.readBoolean returns true
      client.move("a", 2) mustEqual true
      connection.write("MOVE a 2\r\n") was called
    }
    
    "quit" in {
      connection.disconnect returns true
      client.quit mustEqual true
      connection.write("QUIT\r\n") was called
      connection.disconnect was called
    }
    
    "auth with the server" in {
      connection.readBoolean returns true
      client.auth("secret") mustEqual true
      connection.write("AUTH secret\r\n") was called
    }
  }
}
