package redis.jni;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;

/**
 * TODO: Edit this
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
      new Thread() {
        @Override
        public void run() {
          Redis.start(tempFile.getAbsolutePath());
        }
      }.start();
    } catch (Exception e) {
      e.printStackTrace();
    }
  }

  public native static void start(String configFile);
  public native static void init();

  public native void command(byte[]... parameters);

  public static void main(String[] args) throws InterruptedException {
    Thread.sleep(1000);
    init();
    Redis redis = new Redis();
    redis.command("set".getBytes(), "test".getBytes(), "jni".getBytes());
  }

  public static int findFreePort()
      throws IOException {
    ServerSocket server = new ServerSocket(0);
    int port = server.getLocalPort();
    server.close();
    return port;
  }
}
