# Inspired by rabbitmq.rake the Redbox project at http://github.com/rick/redbox/tree/master
require 'fileutils'

class RedisRunner
  
  def self.redisdir
    "/tmp/redis/"
  end
  
  def self.redisconfdir
    '/etc/redis.conf'
  end

  def self.dtach_socket
    '/tmp/redis.dtach'
  end

  # Just check for existance of dtach socket
  def self.running?
    File.exists? dtach_socket
  end
  
  def self.start
    puts 'Detach with Ctrl+\  Re-attach with rake redis:attach'
    sleep 3
    exec "dtach -A #{dtach_socket} redis-server #{redisconfdir}"
  end
  
  def self.attach
    exec "dtach -a #{dtach_socket}"
  end
  
  def self.stop
    sh 'killall redis-server'
  end

end

namespace :redis do
  
  desc 'About redis'
  task :about do
    puts "\nSee http://code.google.com/p/redis/ for information about redis.\n\n"
  end
  
  desc 'Start redis'
  task :start do
    RedisRunner.start
  end
  
  desc 'Stop redis'
  task :stop do
    RedisRunner.stop
  end

  desc 'Attach to redis dtach socket'
  task :attach do
    RedisRunner.attach
  end
  
  desc 'Install the lastest redis from svn'
  task :install => [:about, :download, :make] do
    sh 'sudo cp /tmp/redis/redis-server /usr/bin/'
    sh 'sudo cp /tmp/redis/redis-benchmark /usr/bin/'
    puts 'Installed redis-server and redis-benchmark to /usr/bin/'
    unless File.exists?('/etc/redis.conf')
      sh 'sudo cp /tmp/redis/redis.conf /etc/'
      puts "Installed redis.conf to /etc/ \n You should look at this file!"
    end  
  end
  
  task :make do
    sh "cd #{RedisRunner.redisdir} && make clean"
    sh "cd #{RedisRunner.redisdir} && make"
  end
  
  desc "Download package"
  task :download do
    system 'svn checkout http://redis.googlecode.com/svn/trunk /tmp/redis' unless File.exists?(RedisRunner.redisdir)
    system 'svn up' if File.exists?("#{RedisRunner.redisdir}/.svn")
  end    

end

namespace :dtach do
  
  desc 'About dtach'
  task :about do
    puts "\nSee http://dtach.sourceforge.net/ for information about dtach.\n\n"
  end
  
  desc 'Install dtach 0.8 from source'
  task :install => [:about] do
    
    Dir.chdir('/tmp/')
    unless File.exists?('/tmp/dtach-0.8.tar.gz')
      require 'net/http'
      
      Net::HTTP.start('superb-west.dl.sourceforge.net') do |http|
        resp = http.get('/sourceforge/dtach/dtach-0.8.tar.gz')
        open('/tmp/dtach-0.8.tar.gz', 'wb') do |file| file.write(resp.body) end
      end
    end

    unless File.directory?('/tmp/dtach-0.8')    
      system('tar xzf dtach-0.8.tar.gz')
    end
    
    Dir.chdir('/tmp/dtach-0.8/')
    sh 'cd /tmp/dtach-0.8/ && ./configure && make'    
    sh 'sudo cp /tmp/dtach-0.8/dtach /usr/bin/'
    
    puts 'Dtach successfully installed to /usr/bin.'
  end
end
  