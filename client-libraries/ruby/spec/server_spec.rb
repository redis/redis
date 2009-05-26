require File.dirname(__FILE__) + '/spec_helper'

describe "Server" do
  before(:each) do
    @server = Server.new 'localhost', '6379'
  end

  it "should checkout active connections" do
    threads = []
    10.times do
      threads << Thread.new do
        lambda {
          socket = @server.socket
          socket.close
          socket.write("INFO\r\n")
          socket.read(1)
        }.should_not raise_error(Exception)
      end
    end
  end

end
