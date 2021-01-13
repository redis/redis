# gendoc.rb -- Converts the top-comments inside module.c to modules API
#              reference documentation in markdown format.

# Convert the C comment to markdown
def markdown(s)
    s = s.gsub(/\*\/$/,"")
    s = s.gsub(/^ ?\* ?/,"")
    s = s.gsub(/^\/\*\*? ?/,"")
    s.chop! while s[-1] == "\n" || s[-1] == " "
    lines = s.split("\n")
    newlines = []
    # Backquote function, macro and type names, except if already backquoted and
    # in code blocks indented by 4 spaces.
    lines.each{|l|
        if not l.start_with?('    ')
            l = l.gsub(/(?<!`)RM_[A-z()]+/){|x| "`#{x}`"}
            l = l.gsub(/(?<!`)RedisModule[A-z()]+/){|x| "`#{x}`"}
            l = l.gsub(/(?<!`)REDISMODULE_[A-z]+/){|x| "`#{x}`"}
        end
        newlines << l
    }
    return newlines.join("\n")
end

# Given the source code array and the index at which an exported symbol was
# detected, extracts and outputs the documentation.
def docufy(src,i)
    m = /RM_[A-z0-9]+/.match(src[i])
    name = m[0]
    name = name.sub("RM_","RedisModule_")
    proto = src[i].sub("{","").strip+";\n"
    proto = proto.sub("RM_","RedisModule_")
    puts "## `#{name}`\n\n"
    puts "    #{proto}\n"
    comment = ""
    while true
        i = i-1
        comment = src[i]+comment
        break if src[i] =~ /\/\*/
    end
    comment = markdown(comment)
    puts comment+"\n\n"
end

puts "# Modules API reference\n\n"
puts "<!-- This file is generated from module.c using gendoc.rb -->\n\n"
src = File.open("../module.c").to_a
src.each_with_index{|line,i|
    if line =~ /RM_/ && line[0] != ' ' && line[0] != '#' && line[0] != '/'
        if src[i-1] =~ /\*\//
            docufy(src,i)
        end
    end
}
