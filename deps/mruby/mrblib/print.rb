##
# Kernel
#
# ISO 15.3.1
module Kernel
  unless Kernel.respond_to?(:__printstr__)
    def print(*a)
      raise NotImplementedError.new('print not available')
    end
    def puts(*a)
      raise NotImplementedError.new('puts not available')
    end
    def p(*a)
      raise NotImplementedError.new('p not available')
    end
    def printf(*args)
      raise NotImplementedError.new('printf not available')
    end
  else
    unless Kernel.respond_to?(:sprintf)
      def printf(*args)
        raise NotImplementedError.new('printf not available')
      end
      def sprintf(*args)
        raise NotImplementedError.new('sprintf not available')
      end
    end


    ##
    # Invoke method +print+ on STDOUT and passing +*args+
    #
    # ISO 15.3.1.2.10
    def print(*args)
      i = 0
      len = args.size
      while i < len
        __printstr__ args[i].to_s
        i += 1
      end
    end

    ##
    # Invoke method +puts+ on STDOUT and passing +*args*+
    #
    # ISO 15.3.1.2.11
    def puts(*args)
      i = 0
      len = args.size
      while i < len
        s = args[i].to_s
        __printstr__ s
        __printstr__ "\n" if (s[-1] != "\n")
        i += 1
      end
      __printstr__ "\n" if len == 0
      nil
    end

    ##
    # Print human readable object description
    #
    # ISO 15.3.1.3.34
    def p(*args)
      i = 0
      len = args.size
      while i < len
        __printstr__ args[i].inspect
        __printstr__ "\n"
        i += 1
      end
      args[0]
    end

    def printf(*args)
      __printstr__(sprintf(*args))
      nil
    end
  end
end
