
assert('<', '15.3.3.2.1') do
  class Foo
    include Comparable
    def <=>(x)
      0
    end
  end

  (Foo.new < Foo.new) == false
end

assert('<=', '15.3.3.2.2') do
  class Foo
    include Comparable
    def <=>(x)
      0
    end
  end

  (Foo.new <= Foo.new) == true
end

assert('==', '15.3.3.2.3') do
  class Foo
    include Comparable
    def <=>(x)
      0
    end
  end

  (Foo.new == Foo.new) == true
end

assert('>', '15.3.3.2.4') do
  class Foo
    include Comparable
    def <=>(x)
      0
    end
  end

  (Foo.new > Foo.new) == false
end

assert('>=', '15.3.3.2.5') do
  class Foo
    include Comparable
    def <=>(x)
      0
    end
  end

  (Foo.new >= Foo.new) == true
end

