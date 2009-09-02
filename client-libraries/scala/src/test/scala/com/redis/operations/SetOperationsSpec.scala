import org.specs._
import com.redis._

import org.specs.mock.Mockito
import org.mockito.Mock._
import org.mockito.Mockito._
import org.mockito.Mockito.doNothing

object SetOperationsSpec extends Specification with Mockito {

  "Redis Client Set Operations" should {
    var client: RedisTestClient = null
    var connection: Connection = null
    
    doBefore{
      connection = mock[Connection]
      client = new RedisTestClient(connection)
    }
    
    "add a member to a set" in {
      connection.readBoolean returns true
      client.setAdd("set", "value") must beTrue
      connection.write("SADD set 5\r\nvalue\r\n") was called
    }
    
    "remove an member from a set" in {
      connection.readBoolean returns true
      client.setDelete("set", "value") must beTrue
      connection.write("SREM set 5\r\nvalue\r\n") was called
    }
    
    "return the number of elements in the set" in {
      connection.readInt returns 5
      client.setCount("set") mustEqual 5
      connection.write("SCARD set\r\n") was called
    }
    
    "return all the members from a set" in {
      val setResult = Set("one", "two", "three")
      connection.readSet returns setResult
      client.setMembers("set") mustEqual setResult
      connection.write("SMEMBERS set\r\n") was called
    }
    
    "pop an element from the set" in {
      connection.readString returns "one"
      client.setPop("set") mustEqual "one"
      connection.write("SPOP set\r\n") was called
    }
    
    "move an element from one set to another" in {
      connection.readBoolean returns true
      client.setMove("set", "toset", "value") mustEqual true
      connection.write("SMOVE set toset value\r\n") was called
    }
    
    "tell if member exists on the set" in {
      connection.readBoolean returns true
      client.setMemberExists("set", "value") mustEqual true
      connection.write("SISMEMBER set 5\r\nvalue\r\n") was called
    }
    
    "return the intersection between N sets" in {
      val setResult = Set("one", "two", "three")
      connection.readSet returns setResult
      client.setIntersect("set", "otherset", "another") mustEqual setResult
      connection.write("SINTER set otherset another\r\n") was called
    }
    
    "return the intersection between N sets and store it a new one" in {
      connection.readBoolean returns true
      client.setInterStore("set", "oneset", "twoset") mustEqual true
      connection.write("SINTERSTORE set oneset twoset\r\n") was called
    }
    
    "return the difference between N sets" in {
      val setResult = Set("one", "two", "three")
      connection.readSet returns setResult
      client.setDiff("set", "oneset", "twoset") mustEqual setResult
      connection.write("SDIFF set oneset twoset\r\n") was called
    }
    
    "return the difference between N sets and store it in a new one" in {
      connection.readBoolean returns true
      client.setDiffStore("newset", "oneset", "twoset") mustEqual true
      connection.write("SDIFFSTORE newset oneset twoset\r\n") was called
    }
    
    "return the union between N sets" in {
      val setResult = Set("one", "two", "three")
      connection.readSet returns setResult
      client.setUnion("set", "oneset", "twoset") mustEqual setResult
      connection.write("SUNION set oneset twoset\r\n") was called
    }
    
    "return the union between N sets and store it in a new one" in {
      connection.readBoolean returns true
      client.setUnionStore("set", "oneset", "twoset") mustEqual true
      connection.write("SUNIONSTORE set oneset twoset\r\n") was called
    }
  }
}