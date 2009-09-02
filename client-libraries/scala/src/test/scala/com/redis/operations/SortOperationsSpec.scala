import org.specs._
import com.redis._

import org.specs.mock.Mockito
import org.mockito.Mock._
import org.mockito.Mockito._

object SortOperationsSpec extends Specification with Mockito {
  
  "Redis Client Sort Operations" should {
    
    var client: RedisTestClient = null
    var connection: Connection = null
    
    doBefore{
      connection = mock[Connection]
      client = new RedisTestClient(connection)
    }
    
    "sort the contents of the specified key" in {
      val listResult: List[String] = List("one", "two", "three")
      connection.readList returns listResult
      client.sort("set", "ALPHA DESC") mustEqual listResult
      connection.write("SORT set ALPHA DESC\r\n") was called
    }
    
    "sort the contents of the specified key with default" in {
      val listResult: List[String] = List("one", "two", "three")
      connection.readList returns listResult
      client.sort("set") mustEqual listResult
      connection.write("SORT set\r\n") was called
    }
  }
}
