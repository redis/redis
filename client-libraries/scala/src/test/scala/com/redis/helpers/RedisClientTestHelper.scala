import org.specs._
import com.redis._
import com.redis.operations._

import org.specs.mock.Mockito
import org.mockito.Mock._
import org.mockito.Mockito._
import org.mockito.Mockito.doNothing

class RedisTestClient(val connection: Connection) extends Operations with ListOperations with SetOperations with NodeOperations with KeySpaceOperations with SortOperations {
  var db: Int = 0
  def getConnection(key: String): Connection = connection
}