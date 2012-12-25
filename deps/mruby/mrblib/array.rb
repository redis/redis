##
# Array
#
# ISO 15.2.12
class Array

  ##
  # Calls the given block for each element of +self+
  # and pass the respective element.
  #
  # ISO 15.2.12.5.10
  def each(&block)
    idx, length = -1, self.length-1
    while(idx < length)
      block.call(self[idx += 1])
    end
    self
  end

  ##
  # Calls the given block for each element of +self+
  # and pass the index of the respective element.
  #
  # ISO 15.2.12.5.11
  def each_index(&block)
    idx = 0
    while(idx < length)
      block.call(idx)
      idx += 1
    end
    self
  end

  ##
  # Calls the given block for each element of +self+
  # and pass the respective element. Each element will
  # be replaced by the resulting values.
  #
  # ISO 15.2.12.5.7
  def collect!(&block)
    self.each_index{|idx|
      self[idx] = block.call(self[idx])
    }
    self
  end

  ##
  # Alias for collect!
  #  
  # ISO 15.2.12.5.20
  alias map! collect!

  ##
  # Private method for Array creation.
  #
  # ISO 15.2.12.5.15
  def initialize(size=0, obj=nil, &block)
    raise TypeError, "expected Integer for 1st argument" unless size.kind_of? Integer
    raise ArgumentError, "negative array size" if size < 0

    self.clear
    if size > 0
      self[size - 1] = nil  # allocate

      idx = 0
      while(idx < size)
        self[idx] = (block)? block.call(idx): obj
        idx += 1
      end
    end

    self
  end

  ##
  # Delete element with index +key+
  def delete(key, &block)
    while i = self.index(key)
      self.delete_at(i)
      ret = key
    end
    if ret == nil && block
      block.call
    else
      ret
    end
  end
end

##
# Array is enumerable and comparable
module Enumerable; end
module Comparable; end
class Array
  # ISO 15.2.12.3
  include Enumerable
  include Comparable

  ##
  # Sort all elements and replace +self+ with these
  # elements.
  def sort!(&block)
    self.replace(self.sort(&block))
  end
end
