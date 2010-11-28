#!/usr/bin/env ruby

def argument arg
  name = arg["name"].is_a?(Array) ? arg["name"].join(" ") : arg["name"]
  name = arg["enum"].join "|" if "enum" == arg["type"]
  name = arg["command"] + " " + name if arg["command"]
  if arg["multiple"]
    name = "#{name} [#{name} ...]"
  end
  if arg["optional"]
    name = "[#{name}]"
  end
  name
end

def arguments command
  return "-" unless command["arguments"]
  command["arguments"].map do |arg|
    argument arg
  end.join " "
end

def commands
  return @commands if @commands

  require "net/http"
  require "net/https"
  require "json"
  require "uri"

  url = URI.parse "https://github.com/antirez/redis-doc/raw/master/commands.json"
  client = Net::HTTP.new url.host, url.port
  client.use_ssl = true
  response = client.get url.path
  if response.is_a?(Net::HTTPSuccess)
    @commands = JSON.parse(response.body)
  else
    response.error!
  end
end

def generate_commands
  commands.to_a.sort do |x,y|
    x[0] <=> y[0]
  end.map do |key, command|
    <<-SPEC
{ "#{key}",
    "#{arguments(command)}",
    "#{command["summary"]}",
    COMMAND_GROUP_#{command["group"].upcase},
    "#{command["since"]}" }
    SPEC
  end.join(",  ")
end

# Write to stdout
tmpl = File.read "./utils/help.h"
puts "\n// Auto-generated, do not edit.\n" + tmpl.sub("__COMMANDS__", generate_commands)

