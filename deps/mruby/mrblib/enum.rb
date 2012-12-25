##
# Enumerable
#
#  ISO 15.3.2
#
#  The <code>Enumerable</code> mixin provides collection classes with
#  several traversal and searching methods, and with the ability to
#  sort. The class must provide a method <code>each</code>, which
#  yields successive members of the collection. If
#  <code>Enumerable#max</code>, <code>#min</code>, or
#  <code>#sort</code> is used, the objects in the collection must also
#  implement a meaningful <code><=></code> operator, as these methods
#  rely on an ordering between members of the collection.

module Enumerable

  ##
  # Call the given block for each element
  # which is yield by +each+. Return false
  # if one block value is false. Otherwise
  # return true. If no block is given and
  # +self+ is false return false.
  #
  # ISO 15.3.2.2.1
  def all?(&block)
    st = true
    if block
      self.each{|val|
        unless block.call(val)
          st = false
          break
        end
      }
    else
      self.each{|val|
        unless val
          st = false
          break
        end
      }
    end
    st
  end

  ##
  # Call the given block for each element
  # which is yield by +each+. Return true
  # if one block value is true. Otherwise
  # return false. If no block is given and
  # +self+ is true object return true.
  #
  # ISO 15.3.2.2.2
  def any?(&block)
    st = false
    if block
      self.each{|val|
        if block.call(val)
          st = true
          break
        end
      }
    else
      self.each{|val|
        if val
          st = true
          break
        end
      }
    end
    st
  end

  ##
  # Call the given block for each element
  # which is yield by +each+. Append all
  # values of each block together and 
  # return this value.
  #
  # ISO 15.3.2.2.3
  def collect(&block)
    ary = []
    self.each{|val|
      ary.push(block.call(val))
    }
    ary
  end

  ##
  # Call the given block for each element
  # which is yield by +each+. Return
  # +ifnone+ if no block value was true.
  # Otherwise return the first block value 
  # which had was true.
  #
  # ISO 15.3.2.2.4
  def detect(ifnone=nil, &block)
    ret = ifnone
    self.each{|val|
      if block.call(val)
        ret = val
        break
      end
    }
    ret
  end

  ##
  # Call the given block for each element
  # which is yield by +each+. Pass an
  # index to the block which starts at 0
  # and increase by 1 for each element.
  #
  # ISO 15.3.2.2.5
  def each_with_index(&block)
    i = 0
    self.each{|val|
      block.call(val, i)
      i += 1
    }
    self
  end

  ##
  # Return an array of all elements which
  # are yield by +each+.
  #
  # ISO 15.3.2.2.6
  def entries
    ary = []
    self.each{|val|
      ary.push val
    }
    ary
  end

  ##
  # Alias for find
  #
  # ISO 15.3.2.2.7
  alias find detect

  ##
  # Call the given block for each element
  # which is yield by +each+. Return an array
  # which contains all elements whose block
  # value was true.
  #
  # ISO 15.3.2.2.8
  def find_all(&block)
    ary = []
    self.each{|val|
      ary.push(val) if block.call(val)
    }
    ary
  end

  ##
  # Call the given block for each element
  # which is yield by +each+ and which return
  # value was true when invoking === with
  # +pattern+. Return an array with all 
  # elements or the respective block values. 
  #
  # ISO 15.3.2.2.9
  def grep(pattern, &block)
    ary = []
    self.each{|val|
      if pattern === val
        ary.push((block)? block.call(val): val)
      end
    }
    ary
  end

  ##
  # Return true if at least one element which
  # is yield by +each+ returns a true value
  # by invoking == with +obj+. Otherwise return
  # false.
  #
  # ISO 15.3.2.2.10
  def include?(obj)
    st = false
    self.each{|val|
      if val == obj
        st = true
        break
      end
    }
    st
  end

  ##
  # Call the given block for each element
  # which is yield by +each+. Return value
  # is the sum of all block values. Pass
  # to each block the current sum and the
  # current element.
  #
  # ISO 15.3.2.2.11
  def inject(*args, &block)
    raise ArgumentError, "too many arguments" if args.size > 2
    if Symbol === args[-1]
      sym = args[-1]
      block = ->(x,y){x.send(sym,y)}
      args.pop
    end
    if args.empty?
      flag = true  # no initial argument
      result = nil
    else
      flag = false
      result = args[0]
    end
    self.each{|val|
      if flag
        # push first element as initial
        flag = false
        result = val
      else
        result = block.call(result, val)
      end
    }
    result
  end
  alias reduce inject

  ##
  # Alias for collect
  #
  # ISO 15.3.2.2.12
  alias map collect

  ##
  # Return the maximum value of all elements
  # yield by +each+. If no block is given <=>
  # will be invoked to define this value. If
  # a block is given it will be used instead.
  #
  # ISO 15.3.2.2.13
  def max(&block)
    flag = true  # 1st element?
    result = nil
    self.each{|val|
      if flag
        # 1st element
        result = val
        flag = false
      else
        if block
          result = val if block.call(val, result) > 0
        else
          result = val if (val <=> result) > 0
        end
      end
    }
    result
  end

  ##
  # Return the minimum value of all elements
  # yield by +each+. If no block is given <=>
  # will be invoked to define this value. If
  # a block is given it will be used instead.
  #
  # ISO 15.3.2.2.14
  def min(&block)
    flag = true  # 1st element?
    result = nil
    self.each{|val|
      if flag
        # 1st element
        result = val
        flag = false
      else
        if block
          result = val if block.call(val, result) < 0
        else
          result = val if (val <=> result) < 0
        end
      end
    }
    result
  end

  ##
  # Alias for include?
  #
  # ISO 15.3.2.2.15
  alias member? include?

  ##
  # Call the given block for each element
  # which is yield by +each+. Return an 
  # array which contains two arrays. The
  # first array contains all elements 
  # whose block value was true. The second
  # array contains all elements whose
  # block value was false.
  #
  # ISO 15.3.2.2.16
  def partition(&block)
    ary_T = []
    ary_F = []
    self.each{|val|
      if block.call(val)
        ary_T.push(val)
      else
        ary_F.push(val)
      end
    }
    [ary_T, ary_F]
  end

  ##
  # Call the given block for each element
  # which is yield by +each+. Return an
  # array which contains only the elements
  # whose block value was false.
  #
  # ISO 15.3.2.2.17
  def reject(&block)
    ary = []
    self.each{|val|
      ary.push(val) unless block.call(val)
    }
    ary
  end

  ##
  # Alias for find_all.
  #
  # ISO 15.3.2.2.18
  alias select find_all

  ##
  # TODO
  # Does this OK? Please test it.
  def __sort_sub__(sorted, work, src_ary, head, tail, &block)
    if head == tail
      sorted[head] = work[head] if src_ary == 1
      return
    end

    # on current step, which is a src ary?
    if src_ary == 0
      src, dst = sorted, work
    else
      src, dst = work, sorted
    end

    key = src[head]    # key value for dividing values
    i, j = head, tail  # position to store on the dst ary

    (head + 1).upto(tail){|idx|
      if ((block)? block.call(src[idx], key): (src[idx] <=> key)) > 0
        # larger than key
        dst[j] = src[idx]
        j -= 1
      else
        dst[i] = src[idx]
        i += 1
      end
    }

    sorted[i] = key

    # sort each sub-array
    src_ary = (src_ary + 1) % 2  # exchange a src ary
    __sort_sub__(sorted, work, src_ary, head, i - 1, &block) if i > head
    __sort_sub__(sorted, work, src_ary, i + 1, tail, &block) if i < tail
  end
#  private :__sort_sub__

  ##
  # Return a sorted array of all elements
  # which are yield by +each+. If no block
  # is given <=> will be invoked on each
  # element to define the order. Otherwise
  # the given block will be used for
  # sorting.
  #
  # ISO 15.3.2.2.19
  def sort(&block)
    ary = []
    self.each{|val| ary.push(val)}
    unless ary.empty?
      __sort_sub__(ary, ::Array.new(ary.size), 0, 0, ary.size - 1, &block)
    end
    ary
  end

  ##
  # Alias for entries.
  #
  # ISO 15.3.2.2.20
  alias to_a entries
end
