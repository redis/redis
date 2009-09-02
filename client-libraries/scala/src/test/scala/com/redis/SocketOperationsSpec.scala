import org.specs._
import com.redis._

import java.io._
import java.net.Socket

import org.specs.mock.Mockito
import org.mockito.Mock._
import org.mockito.Mockito._

class SocketOperationTest(val host:String, val port: Int) extends SocketOperations

object SocketOperationsSpec extends Specification with Mockito {
  
  "Socket Operations" should {
    var socketOperation: SocketOperationTest = null
    var socket: Socket = null
    var in: BufferedReader = null
    
    doBefore {
      socketOperation = new SocketOperationTest("localhost", 6379666)
      socket = mock[Socket]
      in = mock[BufferedReader]
      socketOperation.socket = socket
      socketOperation.in = in
    }
    
    def readOkFromInput = { when(in.readLine()).thenReturn(socketOperation.OK) }
    def readSingleFromInput = { when(in.readLine()).thenReturn(socketOperation.SINGLE) }
    def readBulkFromInput = { in.readLine() returns("$6\r\nfoobar\r\n") thenReturns("$6\r\nfoobar\r\n")  }
    def readIntFromInput = { when(in.readLine()).thenReturn(socketOperation.INT+"666") }
    
    "tell if it's connected" in {
      socketOperation.connected mustEqual true
      socketOperation.socket = null
      socketOperation.connected mustEqual false
    }
    
    "return false when can't connect" in {
      socketOperation.connect mustEqual false
    }
    
    "return current data input stream" in {
      socketOperation.getInputStream mustEqual in
    }
    
    "read a line from socket" in {
      readOkFromInput
      socketOperation.in mustEqual in
      socketOperation.readline mustEqual socketOperation.OK
    }
    
    "read type response" in {
      readOkFromInput
      socketOperation.readtype mustEqual ("+", socketOperation.OK)
    }
    
    "when reading responses" in {
      
      "read OK" in {
        readOkFromInput
        socketOperation.readResponse mustEqual socketOperation.OK
      }
      
      "read single line" in {
        readSingleFromInput
        socketOperation.readResponse mustEqual socketOperation.SINGLE
      }
      
      "reconnect on error" in {
        socketOperation.readResponse mustEqual false
        socket.close was called
        socketOperation.connected mustEqual true
      }
      
      "read in bulk" in {
        // readBulkFromInput
        // this shouldn't be the response, it doesn't seem to work return and then returns.
        // Here's what should happen: '$6\r\n' on first readLine and then 'foobar\r\n'
        readBulkFromInput
        socketOperation.readtype mustEqual ("$", "$6\r\nfoobar\r\n")
        socketOperation.readResponse mustEqual "$6\r\nfoobar\r\n"
        socketOperation.bulkReply("$6\r\nfoobar\r\n") was called
      }
      
      "read integer" in {
        readIntFromInput
        socketOperation.readInt mustEqual 666
      }
      
      "read a boolean return value" in {
        readOkFromInput
        socketOperation.readBoolean mustEqual true
      }
    }
  }
}
