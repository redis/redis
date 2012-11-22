##
# Exception
#
# ISO 15.2.22
class Exception

  ##
  # Raise an exception.
  #
  # ISO 15.2.22.4.1
  def self.exception(*args, &block)
    self.new(*args, &block)
  end
end

# ISO 15.2.24
class ArgumentError < StandardError
end

# ISO 15.2.25
class LocalJumpError < StandardError
end

# ISO 15.2.26
class RangeError < StandardError
end

class FloatDomainError < RangeError
end

# ISO 15.2.26
class RegexpError < StandardError
end

# ISO 15.2.29
class TypeError < StandardError
end

# ISO 15.2.31
class NameError < StandardError
end

# ISO 15.2.32
class NoMethodError < NameError
end

# ISO 15.2.33
class IndexError < StandardError
end

class KeyError < IndexError
end

class NotImplementedError < ScriptError
end

