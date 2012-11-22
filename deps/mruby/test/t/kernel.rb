##
# Kernel ISO Test

assert('Kernel', '15.3.1') do
  Kernel.class == Module
end

assert('Kernel.block_given?', '15.3.1.2.2') do
  def bg_try(&b)
    if Kernel.block_given?
      yield
    else
      "no block"
    end
  end

  (Kernel.block_given? == false) and
    # test without block
    (bg_try == "no block") and
    # test with block
    ((bg_try { "block" }) == "block") and
    # test with block
    ((bg_try do "block" end) == "block")
end

assert('Kernel.global_variables', '15.3.1.2.4') do
  Kernel.global_variables.class == Array
end

assert('Kernel.iterator?', '15.3.1.2.5') do
  Kernel.iterator? == false
end

assert('Kernel.lambda', '15.3.1.2.6') do
  l = Kernel.lambda do
    true
  end

  m = Kernel.lambda(&l)

  l.call and l.class == Proc and m.call and m.class == Proc
end

# Not implemented at the moment
#assert('Kernel.local_variables', '15.3.1.2.7') do
#  Kernel.local_variables.class == Array
#end

assert('Kernel.loop', '15.3.1.2.8') do
  i = 0

  Kernel.loop do
    i += 1
    break if i == 100
  end

  i == 100
end

assert('Kernel.p', '15.3.1.2.9') do
  # TODO search for a way to test p to stdio
  true
end

assert('Kernel.print', '15.3.1.2.10') do
  # TODO search for a way to test print to stdio
  true
end

assert('Kernel.puts', '15.3.1.2.11') do
  # TODO search for a way to test puts to stdio
  true
end

assert('Kernel.raise', '15.3.1.2.12') do
  e_list = []

  begin
    Kernel.raise
  rescue => e
    e_list << e
  end

  begin
    Kernel.raise RuntimeError.new
  rescue => e
    e_list << e
  end

  # result without argument
  e_list[0].class == RuntimeError and
    # result with RuntimeError argument
    e_list[1].class == RuntimeError
end

assert('Kernel#__id__', '15.3.1.3.3') do
  __id__.class == Fixnum
end

assert('Kernel#__send__', '15.3.1.3.4') do
  # test with block
  l = __send__(:lambda) do
    true
  end
 
  l.call and l.class == Proc and
    # test with argument
    __send__(:respond_to?, :nil?) and
    # test without argument and without block
    __send__(:public_methods).class == Array
end

assert('Kernel#block_given?', '15.3.1.3.6') do
  def bg_try(&b)
    if block_given?
      yield
    else
      "no block"
    end
  end

  (block_given? == false) and
    (bg_try == "no block") and
    ((bg_try { "block" }) == "block") and
    ((bg_try do "block" end) == "block")
end

assert('Kernel#class', '15.3.1.3.7') do
  Kernel.class == Module
end

assert('Kernel#clone', '15.3.1.3.8') do
  class KernelCloneTest
    def initialize
      @v = 0
    end

    def get
      @v
    end

    def set(v)
      @v = v
    end
  end

  a = KernelCloneTest.new
  a.set(1)
  b = a.clone

  def a.test
  end
  a.set(2)
  c = a.clone

  a.get == 2 and b.get == 1 and c.get == 2 &&
    a.respond_to?(:test) == true and
    b.respond_to?(:test) == false and
    c.respond_to?(:test) == true
end

assert('Kernel#dup', '15.3.1.3.9') do
  class KernelDupTest
    def initialize
      @v = 0
    end

    def get
      @v
    end

    def set(v)
      @v = v
    end
  end

  a = KernelDupTest.new
  a.set(1)
  b = a.dup

  def a.test
  end
  a.set(2)
  c = a.dup

  a.get == 2 and b.get == 1 and c.get == 2 and
    a.respond_to?(:test) == true and
    b.respond_to?(:test) == false and
    c.respond_to?(:test) == false
end

assert('Kernel#extend', '15.3.1.3.13') do
  class Test4ExtendClass
  end

  module Test4ExtendModule
    def test_method; end
  end

  a = Test4ExtendClass.new
  a.extend(Test4ExtendModule)
  b = Test4ExtendClass.new

  a.respond_to?(:test_method) == true && b.respond_to?(:test_method) == false
end

assert('Kernel#extend works on toplevel', '15.3.1.3.13') do
  module Test4ExtendModule
    def test_method; end
  end
  # This would crash... 
  extend(Test4ExtendModule)

  respond_to?(:test_method) == true
end

assert('Kernel#global_variables', '15.3.1.3.14') do
  global_variables.class == Array
end

assert('Kernel#hash', '15.3.1.3.15') do
  hash == hash
end

assert('Kernel#inspect', '15.3.1.3.17') do
  s = inspect
  s.class == String and s == "main"
end

assert('Kernel#instance_variables', '15.3.1.3.23') do
  o = Object.new
  o.instance_eval do
    @a = 11
    @b = 12
  end
  ivars = o.instance_variables
  ivars.class == Array and ivars.size == 2 and ivars.include?(:@a) and ivars.include?(:@b)
end

assert('Kernel#is_a?', '15.3.1.3.24') do
  is_a?(Kernel) and not is_a?(Array)
end

assert('Kernel#iterator?', '15.3.1.3.25') do
  iterator? == false
end

assert('Kernel#kind_of?', '15.3.1.3.26') do
  kind_of?(Kernel) and not kind_of?(Array)
end

assert('Kernel#lambda', '15.3.1.3.27') do
  l = lambda do
    true
  end

  m = lambda(&l)

  l.call and l.class == Proc and m.call and m.class == Proc
end

# Not implemented yet
#assert('Kernel#local_variables', '15.3.1.3.28') do
#  local_variables.class == Array
#end

assert('Kernel#loop', '15.3.1.3.29') do
  i = 0

  loop do
    i += 1
    break if i == 100
  end

  i == 100
end

assert('Kernel#methods', '15.3.1.3.31') do
  methods.class == Array
end

assert('Kernel#nil?', '15.3.1.3.32') do
  nil? == false
end

assert('Kernel#object_id', '15.3.1.3.33') do
  object_id.class == Fixnum
end

assert('Kernel#private_methods', '15.3.1.3.36') do
  private_methods.class == Array
end

assert('Kernel#protected_methods', '15.3.1.3.37') do
  protected_methods.class == Array
end

assert('Kernel#public_methods', '15.3.1.3.38') do
  public_methods.class == Array
end

assert('Kernel#raise', '15.3.1.3.40') do
  e_list = []

  begin
    raise
  rescue => e
    e_list << e
  end

  begin
    raise RuntimeError.new
  rescue => e
    e_list << e
  end

  # result without argument
  e_list[0].class == RuntimeError and
    # result with RuntimeError argument
    e_list[1].class == RuntimeError
end

assert('Kernel#respond_to?', '15.3.1.3.43') do
  class Test4RespondTo
    def test_method; end
    undef test_method
  end

  respond_to?(:nil?) and Test4RespondTo.new.respond_to?(:test_method) == false
end

assert('Kernel#send', '15.3.1.3.44') do
  # test with block
  l = send(:lambda) do
    true
  end

  l.call and l.class == Proc and
    # test with argument
    send(:respond_to?, :nil?) and
    # test without argument and without block
    send(:public_methods).class == Array
end

assert('Kernel#singleton_methods', '15.3.1.3.45') do
  singleton_methods.class == Array
end

assert('Kernel#to_s', '15.3.1.3.46') do
  to_s.class == String
end
