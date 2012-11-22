assert('super', '11.3.4') do
  test = false
  begin
    super
  rescue NoMethodError
    test = true
  end

  class SuperFoo
    def foo
      true
    end
    def bar(*a)
      a
    end
  end
  class SuperBar < SuperFoo
    def foo
      super
    end
    def bar(*a)
      super(*a)
    end
  end
  bar = SuperBar.new
  test &&= bar.foo
  test &&= (bar.bar(1,2,3) == [1,2,3])
  test
end

assert('yield', '11.3.5') do
  begin
    yield
  rescue LocalJumpError
    true
  else
    false
  end
end

assert('Abbreviated variable assignment', '11.4.2.3.2') do
  a ||= 1
  b &&= 1
  c = 1
  c += 2
  a == 1 and b == nil and c == 3
end

assert('Nested const reference') do
  module Syntax4Const
    CONST1 = "hello world"
    class Const2
      def const1
        CONST1
      end
    end
  end
  Syntax4Const::CONST1 == "hello world" and
  Syntax4Const::Const2.new.const1 == "hello world"
end

assert('Abbreviated variable assignment as returns') do
  module Syntax4AbbrVarAsgnAsReturns
    class A
      def b
        @c ||= 1
      end
    end
  end
  Syntax4AbbrVarAsgnAsReturns::A.new.b == 1
end
