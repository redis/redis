require 'benchmark'
$:.push File.join(File.dirname(__FILE__), 'lib')
require 'redis'

times = 20000

@r = Redis.new#(:debug => true)
@r['foo'] = "The first line we sent to the server is some text"

Benchmark.bmbm do |x|
  x.report("set") do
    20000.times do |i|
      @r["set#{i}"] = "The first line we sent to the server is some text"; @r["foo#{i}"]
    end
  end
  
  x.report("set (pipelined)") do
    @r.pipelined do |pipeline|
      20000.times do |i|
        pipeline["set_pipelined#{i}"] = "The first line we sent to the server is some text"; @r["foo#{i}"]
      end
    end
  end
  
  x.report("push+trim") do
    20000.times do |i|
      @r.push_head "push_trim#{i}", i
      @r.list_trim "push_trim#{i}", 0, 30
    end
  end
  
  x.report("push+trim (pipelined)") do
    @r.pipelined do |pipeline|
      20000.times do |i|
        pipeline.push_head "push_trim_pipelined#{i}", i
        pipeline.list_trim "push_trim_pipelined#{i}", 0, 30
      end
    end
  end
end

@r.keys('*').each do |k|
  @r.delete k
end