##
# Kernel
#
# ISO 15.3.1
module Kernel
  ##
  # Calls the given block repetitively.
  #
  # ISO 15.3.1.2.8
  def self.loop #(&block)
    while(true)
      yield
    end
  end

  # 15.3.1.2.3
  def self.eval(s)
    raise NotImplementedError.new("eval not implemented")
  end

  # 15.3.1.3.12
  def eval(s)
    Kernel.eval(s)
  end

  ##
  # Alias for +Kernel.loop+.
  #
  # ISO 15.3.1.3.29
  def loop #(&block)
    while(true)
      yield
    end
  end
end
