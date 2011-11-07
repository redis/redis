package redis.jni;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;

/**
 * Native redis bridge.
 * <p/>
 * User: sam
 * Date: 11/6/11
 * Time: 10:10 AM
 */
public class Redis {
  static {
    System.loadLibrary("redis");
    try {
      final File tempFile = File.createTempFile("redis", "conf");
      tempFile.deleteOnExit();
      OutputStream os = new FileOutputStream(tempFile);
      os.write(("port " + findFreePort() + "\n").getBytes());
      byte[] bytes = new byte[8192];
      InputStream is = Redis.class.getResourceAsStream("redis.conf");
      int read;
      while ((read = is.read(bytes)) != -1) {
        os.write(bytes, 0, read);
      }
      os.flush();
      os.close();
      Redis.start(tempFile.getAbsolutePath());
      new Thread() {
        @Override
        public void run() {
          eventloop();
        }
      }.start();
    } catch (Exception e) {
      e.printStackTrace();
      throw new AssertionError("Failed to start Redis");
    }
  }

  public native static void start(String configFile);
  public native static void eventloop();
  public native static synchronized void command(byte[]... parameters);

  public static void main(String[] args) throws InterruptedException {
    command("set".getBytes(), "test".getBytes(), "jni".getBytes());
  }

  public static int findFreePort()
      throws IOException {
    ServerSocket server = new ServerSocket(0);
    int port = server.getLocalPort();
    server.close();
    return port;
  }
}
