import org.specs._
import com.redis._

import org.specs.mock.Mockito
import org.mockito.Mock._
import org.mockito.Mockito._
import org.mockito.Mockito.doNothing

object RedisClusterSpec extends Specification with Mockito {

  "Redis Cluster" should {
    var cluster: RedisCluster = null
    var mockedRedis: Redis = null
    
    doBefore {
      cluster = new RedisCluster("localhost:11221", "localhost:99991")
      mockedRedis = mock[Redis]
    }
    
    "print formatted client status" in {
      cluster.toString must be matching("localhost:11221 <connected:false>, localhost:99991 <connected:false>")
    }
    
    "get the connection for the specified key" in {
      cluster.getConnection("key") mustEqual Connection("localhost", 99991)
      cluster.getConnection("anotherkey") mustEqual Connection("localhost", 11221)
    }
    
    "use the default number of replicas" in {
      cluster.replicas mustEqual 160
    }
    
    "initialize cluster" in {
      val initializedCluster = cluster.initialize_cluster
      initializedCluster.size mustEqual 2
      initializedCluster(0) mustEqual false
      initializedCluster(1) mustEqual false
    }
    
    "connect all the redis instances" in {
      cluster.cluster(1) = mockedRedis
      
      cluster.cluster(1).connect returns true
      val connectResult = cluster.connect
      connectResult.size mustEqual 2
      connectResult(0) mustEqual false
      connectResult(1) mustEqual true
      cluster.cluster(1).connect was called
    }
  }
}
