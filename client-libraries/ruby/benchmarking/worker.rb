BENCHMARK_ROOT = File.dirname(__FILE__)
REDIS_ROOT = File.join(BENCHMARK_ROOT, "..", "lib")

$: << REDIS_ROOT
require 'redis'
require 'benchmark'

def show_usage
  puts <<-EOL
    Usage: worker.rb [read:write] <start_index> <end_index> <sleep_msec>
  EOL
end

def shift_from_argv
  value = ARGV.shift
  unless value
    show_usage
    exit -1
  end
  value
end

operation = shift_from_argv.to_sym
start_index = shift_from_argv.to_i
end_index = shift_from_argv.to_i
sleep_msec = shift_from_argv.to_i
sleep_duration = sleep_msec/1000.0

redis = Redis.new

case operation
  when :initialize

    start_index.upto(end_index) do |i|
      redis[i] = 0
    end

  when :clear

    start_index.upto(end_index) do |i|
      redis.delete(i)
    end

  when :read, :write

    puts "Starting to #{operation} at segment #{end_index + 1}"

    loop do
      t1 = Time.now
      start_index.upto(end_index) do |i|
        case operation
          when :read
            redis.get(i)
          when :write
            redis.incr(i)
          else
            raise "Unknown operation: #{operation}"
        end
        sleep sleep_duration
      end
      t2 = Time.now

      requests_processed = end_index - start_index
      time = t2 - t1
      puts "#{t2.strftime("%H:%M")} [segment #{end_index + 1}] : Processed #{requests_processed} requests in #{time} seconds - #{(requests_processed/time).round} requests/sec"
    end

  else
    raise "Unknown operation: #{operation}"
end

