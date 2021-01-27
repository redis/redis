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
    # Fix some markdown, except in code blocks indented by 4 spaces.
    lines.each{|l|
        if not l.start_with?('    ')
            # Rewrite RM_Xyz() to `RedisModule_Xyz()`. The () suffix is
            # optional. Even RM_Xyz*() with * as wildcard is handled.
            l = l.gsub(/(?<!`)RM_([A-z]+(?:\*?\(\))?)/, '`RedisModule_\1`')
            # Add backquotes around RedisModule functions and type where missing.
            l = l.gsub(/(?<!`)RedisModule[A-z]+(?:\*?\(\))?/){|x| "`#{x}`"}
            # Add backquotes around c functions like malloc() where missing.
            l = l.gsub(/(?<![`A-z])[a-z_]+\(\)/, '`\0`')
            # Add backquotes around macro and var names containing underscores.
            l = l.gsub(/(?<![`A-z\*])[A-Za-z]+_[A-Za-z0-9_]+/){|x| "`#{x}`"}
            # Link URLs preceded by space (i.e. when not already linked)
            l = l.gsub(/ (https?:\/\/[A-Za-z0-9_\/\.\-]+[A-Za-z0-9\/])/,
                       ' [\1](\1)')
        end
        # Link function names to their definition within the page
        l = l.gsub(/`(RedisModule_[A-z0-9]+)[()]*`/) {|x|
            $index[$1] ? "[#{x}](\##{$1})" : x
        }
        newlines << l
    }
    return newlines.join("\n")
end

# Linebreak a prototype longer than 80 characters on the commas, but only
# between balanced parentheses so that we don't linebreak args which are
# function pointers, and then aligning each arg under each other.
def linebreak_proto(proto, indent)
    if proto.bytesize <= 80
        return proto
    end
    parts = proto.split(/,\s*/);
    if parts.length == 1
        return proto;
    end
    align_pos = proto.index("(") + 1;
    align = " " * align_pos
    result = parts.shift;
    last_part = parts.pop;
    bracket_balance = 0;
    parts.each{|part|
        result += part
        bracket_balance += part.count("(") - part.count(")")
        if bracket_balance == 0
            result += ",\n" + indent + align
        else
            result += ", "
        end
    }
    return result + last_part;
end

# Given the source code array and the index at which an exported symbol was
# detected, extracts and outputs the documentation.
def docufy(src,i)
    m = /RM_[A-z0-9]+/.match(src[i])
    name = m[0]
    name = name.sub("RM_","RedisModule_")
    proto = src[i].sub("{","").strip+";\n"
    proto = proto.sub("RM_","RedisModule_")
    proto = linebreak_proto(proto, "    ");
    # Add a link target with the function name. (We don't trust the exact id of
    # the generated one, which depends on the Markdown implementation.)
    puts "<span id=\"#{name}\"></span>\n\n"
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

# Like src.each_with_index but executes the block only for RM_ function
# definition lines preceded by a documentation comment block.
def each_rm_func_line_with_index(src, &block)
    src.each_with_index{|line,i|
        if line =~ /RM_/ && line[0] != ' ' && line[0] != '#' && line[0] != '/'
            if src[i-1] =~ /\*\//
                block.call(line, i)
            end
        end
    }
end

puts "# Modules API reference\n\n"
puts "<!-- This file is generated from module.c using gendoc.rb -->\n\n"
src = File.open("../module.c").to_a

# Build function index
$index = {}
each_rm_func_line_with_index(src){|line,i|
    line =~ /RM_([A-z0-9]+)/
    name = "RedisModule_#{$1}"
    $index[name] = true
}

# Docufy: Print function prototype and markdown docs
each_rm_func_line_with_index(src){|line,i| docufy(src,i)}

puts "## Function index\n\n"
$index.keys.sort.each{|x| puts "* [`#{x}`](\##{x})\n"}
puts "\n"
