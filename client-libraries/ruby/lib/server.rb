##
# This class represents a redis server instance.

class Server

  ##
  # The amount of time to wait before attempting to re-establish a
  # connection with a server that is marked dead.

  RETRY_DELAY = 30.0

  ##
  # The host the redis server is running on.

  attr_reader :host

  ##
  # The port the redis server is listening on.

  attr_reader :port
  
  ##
  #
  
  attr_reader :replica

  ##
  # The time of next retry if the connection is dead.

  attr_reader :retry

  ##
  # A text status string describing the state of the server.

  attr_reader :status

  ##
  # Create a new Redis::Server object for the redis instance
  # listening on the given host and port.

  def initialize(host, port = DEFAULT_PORT)
    raise ArgumentError, "No host specified" if host.nil? or host.empty?
    raise ArgumentError, "No port specified" if port.nil? or port.to_i.zero?

    @host   = host
    @port   = port.to_i

    @sock   = nil
    @retry  = nil
    @status = 'NOT CONNECTED'
    @timeout = 1
  end

  ##
  # Return a string representation of the server object.
  def inspect
    "<Redis::Server: %s:%d (%s)>" % [@host, @port, @status]
  end

  ##
  # Try to connect to the redis server targeted by this object.
  # Returns the connected socket object on success or nil on failure.

  def socket
    return @sock if @sock and not @sock.closed?

    @sock = nil

    # If the host was dead, don't retry for a while.
    return if @retry and @retry > Time.now

    # Attempt to connect if not already connected.
    begin
      @sock = connect_to(@host, @port, @timeout)
      @sock.setsockopt Socket::IPPROTO_TCP, Socket::TCP_NODELAY, 1
      @retry  = nil
      @status = 'CONNECTED'
    rescue Errno::EPIPE, Errno::ECONNREFUSED => e
      puts "Socket died... socket: #{@sock.inspect}\n" if $debug
      @sock.close
      retry
    rescue SocketError, SystemCallError, IOError => err
      puts "Unable to open socket: #{err.class.name}, #{err.message}" if $debug
      mark_dead err
    end

    return @sock
  end

  def connect_to(host, port, timeout=nil)
    addrs = Socket.getaddrinfo(host, nil)
    addr = addrs.detect { |ad| ad[0] == 'AF_INET' }
    sock = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM, 0)
    if timeout
      secs = Integer(timeout)
      usecs = Integer((timeout - secs) * 1_000_000)
      optval = [secs, usecs].pack("l_2")
      sock.setsockopt Socket::SOL_SOCKET, Socket::SO_RCVTIMEO, optval
      sock.setsockopt Socket::SOL_SOCKET, Socket::SO_SNDTIMEO, optval
    end
    sock.connect(Socket.pack_sockaddr_in(port, addr[3]))
    sock
  end

  ##
  # Close the connection to the redis server targeted by this
  # object.  The server is not considered dead.

  def close
    @sock.close if @sock && !@sock.closed?
    @sock   = nil
    @retry  = nil
    @status = "NOT CONNECTED"
  end

  ##
  # Mark the server as dead and close its socket.
  def mark_dead(error)
    @sock.close if @sock && !@sock.closed?
    @sock   = nil
    @retry  = Time.now #+ RETRY_DELAY

    reason = "#{error.class.name}: #{error.message}"
    @status = sprintf "%s:%s DEAD (%s), will retry at %s", @host, @port, reason, @retry
    puts @status
  end

end
