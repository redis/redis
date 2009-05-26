require 'fileutils'

def run_in_background(command)
  fork { system command }
end

def with_all_segments(&block)
  0.upto(9) do |segment_number|
    block_size = 100000
    start_index = segment_number * block_size
    end_index = start_index + block_size - 1
    block.call(start_index, end_index)
  end
end

#with_all_segments do |start_index, end_index|
#  puts "Initializing keys from #{start_index} to #{end_index}"
#  system "ruby worker.rb initialize #{start_index} #{end_index} 0"
#end

with_all_segments do |start_index, end_index|
  run_in_background "ruby worker.rb write #{start_index} #{end_index} 10"
  run_in_background "ruby worker.rb read #{start_index} #{end_index} 1"
end