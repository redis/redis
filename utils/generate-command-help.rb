#!/usr/bin/env ruby

require 'net/http'
require 'net/https'
require 'json'
require 'uri'

dest = ARGV[0]
tmpl = File.read './utils/help.h'

url = URI.parse 'https://github.com/antirez/redis-doc/raw/master/commands.json'
client = Net::HTTP.new url.host, url.port
client.use_ssl = true
res = client.get url.path

def argument arg
  name = arg['name'].is_a?(Array) ? arg['name'].join(' ') : arg['name']
  name = arg['enum'].join '|' if 'enum' == arg['type']
  name = arg['command'] + ' ' + name if arg['command']
  if arg['multiple']
    name = "(#{name})"
    name += arg['optional'] ? '*' : '+'
  elsif arg['optional']
    name = "(#{name})?"
  end
  name
end

def arguments command
  return '-' unless command['arguments']
  command['arguments'].map do |arg|
    argument arg
  end.join ' '
end

case res
when Net::HTTPSuccess
  first = true
  commands = JSON.parse(res.body)
  c = commands.map do |key, command|
    buf = if first
      first = false
      ' '
    else
      "\n  ,"
    end
    buf += " { \"#{key}\"\n" +
    "  , \"#{arguments(command)}\"\n" +
    "  , \"#{command['summary']}\"\n" +
    "  , COMMAND_GROUP_#{command['group'].upcase}\n" +
    "  , \"#{command['since']}\" }"
  end.join("\n")
  puts "\n// Auto-generated, do not edit.\n" + tmpl.sub('__COMMANDS__', c)
else
  res.error!
end