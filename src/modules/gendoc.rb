# gendoc.rb -- Converts the top-comments inside module.c to modules API
#              reference documentaiton in markdown format.

# Convert the C comment to markdown
def markdown(s)
    s = s.gsub(/\*\/$/,"")
    s = s.gsub(/^ \* {0,1}/,"")
    s = s.gsub(/^\/\* /,"")
    if s[0] != ' '
        s = s.gsub(/RM_[A-z()]+/){|x| "`#{x}`"}
        s = s.gsub(/RedisModule_[A-z()]+/){|x| "`#{x}`"}
        s = s.gsub(/REDISMODULE_[A-z]+/){|x| "`#{x}`"}
    end
    s.chop! while s[-1] == "\n" || s[-1] == " "
    return s
end

# Given the source code array and the index at which an exported symbol was
# detected, extracts and outputs the documentation.
def docufy(src,i)
    m = /RM_[A-z0-9]+/.match(src[i])
    proto = src[i].sub("{","").strip+";\n"
    puts "## `#{m[0]}`\n\n"
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
src = File.open("../module.c").to_a
src.each_with_index{|line,i|
    if line =~ /RM_/ && line[0] != ' ' && line[0] != '#' && line[0] != '/'
        if src[i-1] =~ /\*\//
            docufy(src,i)
        end
    end
}
