##
# Struct
#
# ISO 15.2.18

if Object.const_defined?(:Struct)
  class Struct

    ##
    # Calls the given block for each element of +self+
    # and pass the respective element.
    #
    # ISO 15.2.18.4.4
    def each(&block)
      self.class.members.each{|field|
        block.call(self[field])
      }
      self
    end

    ##
    # Calls the given block for each element of +self+
    # and pass the name and value of the respectiev
    # element.
    #
    # ISO 15.2.18.4.5
    def each_pair(&block)
      self.class.members.each{|field|
        block.call(field.to_sym, self[field])
      }
      self
    end

    ##
    # Calls the given block for each element of +self+
    # and returns an array with all elements of which
    # block is not false.
    #
    # ISO 15.2.18.4.7
    def select(&block)
      ary = []
      self.class.members.each{|field|
        val = self[field]
        ary.push(val) if block.call(val)
      }
      ary
    end
  end
end

