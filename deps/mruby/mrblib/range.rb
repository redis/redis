##
# Range
#
# ISO 15.2.14
class Range

  ##
  # Calls the given block for each element of +self+
  # and pass the respective element.
  #
  # ISO 15.2.14.4.4
  def each(&block)
    val = self.first
    unless val.respond_to? :succ
      raise TypeError, "can't iterate"
    end

    last = self.last
    return self if (val <=> last) > 0

    while((val <=> last) < 0)
      block.call(val)
      val = val.succ
    end

    block.call(val) unless exclude_end?

    self
  end
end

##
# Range is enumerable
#
# ISO 15.2.14.3
module Enumerable; end
class Range
  include Enumerable
end
