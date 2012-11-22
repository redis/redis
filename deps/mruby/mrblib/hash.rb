##
# Hash
#
# ISO 15.2.13
class Hash

  ##
  # Delete the element with the key +key+.
  # Return the value of the element if +key+
  # was found. Return nil if nothing was
  # found. If a block is given, call the
  # block with the value of the element.
  #
  # ISO 15.2.13.4.8
  def delete(key, &block)
    if block && ! self.has_key?(key)
      block.call(key)
    else
      self.__delete(key)
    end
  end

  ##
  # Calls the given block for each element of +self+
  # and pass the key and value of each element.
  #
  # ISO 15.2.13.4.9
  def each(&block)
    self.keys.each{|k| block.call([k, self[k]])}
    self
  end

  ##
  # Calls the given block for each element of +self+
  # and pass the key of each element.
  #
  # ISO 15.2.13.4.10
  def each_key(&block)
    self.keys.each{|k| block.call(k)}
    self
  end

  ##
  # Calls the given block for each element of +self+
  # and pass the value of each element.
  #
  # ISO 15.2.13.4.11
  def each_value(&block)
    self.keys.each{|k| block.call(self[k])}
    self
  end

  ##
  # Create a direct instance of the class Hash.
  #
  # ISO 15.2.13.4.16
  def initialize(*args, &block)
    self.__init_core(block, *args)
  end

  ##
  # Return a hash which contains the content of
  # +self+ and +other+. If a block is given
  # it will be called for each element with
  # a duplicate key. The value of the block
  # will be the final value of this element.
  # 
  # ISO 15.2.13.4.22
  def merge(other, &block)
    h = {}
    raise "can't convert argument into Hash" unless other.respond_to?(:to_hash)
    other = other.to_hash
    self.each_key{|k| h[k] = self[k]}
    if block
      other.each_key{|k|
        h[k] = (self.has_key?(k))? block.call(k, self[k], other[k]): other[k]
      }
    else
      other.each_key{|k| h[k] = other[k]}
    end
    h
  end

  # 1.8/1.9 Hash#reject! returns Hash; ISO says nothing.
  def reject!(&b)
    keys = []
    self.each_key{|k|
      v = self[k]
      if b.call(k, v)
        keys.push(k)
      end
    }
    return nil if keys.size == 0
    keys.each{|k|
      self.delete(k)
    }
    self
  end

  # 1.8/1.9 Hash#reject returns Hash; ISO says nothing.
  def reject(&b)
    h = {}
    self.each_key{|k|
      v = self[k]
      unless b.call(k, v)
        h[k] = v
      end
    }
    h
  end

  # 1.9 Hash#select! returns Hash; ISO says nothing.
  def select!(&b)
    keys = []
    self.each_key{|k|
      v = self[k]
      unless b.call(k, v)
        keys.push(k)
      end
    }
    return nil if keys.size == 0
    keys.each{|k|
      self.delete(k)
    }
    self
  end

  # 1.9 Hash#select returns Hash; ISO says nothing.
  def select(&b)
    h = {}
    self.each_key{|k|
      v = self[k]
      if b.call(k, v)
        h[k] = v
      end
    }
    h
  end
end

##
# Hash is enumerable
#
# ISO 15.2.13.3
module Enumerable; end
class Hash
  include Enumerable
end
