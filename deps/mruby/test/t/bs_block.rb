##
# Bootstrap tests for blocks

assert('BS Block 1') do
  1.times{
    begin
      a = 1
    ensure
      foo = nil
    end
  } == 1
end

assert('BS Block 2') do
  [1,2,3].find{|x| x == 2} == 2
end

assert('BS Block 3') do
  class E
    include Enumerable
    def each(&block)
      [1, 2, 3].each(&block)
    end
  end
  E.new.find {|x| x == 2 } == 2
end

assert('BS Block 3') do
  sum = 0
  for x in [1, 2, 3]
    sum += x
  end
  sum == 6
end

assert('BS Block 4') do
  sum = 0
  for x in (1..5)
    sum += x
  end
  sum == 15
end

assert('BS Block 5') do
  sum = 0
  for x in []
    sum += x
  end
  sum == 0
end

assert('BS Block 6') do
  ans = []
  1.times{
    for n in 1..3
      a = n
      ans << a
    end
  } == 1
end

assert('BS Block 7') do
  ans = []
  for m in 1..3
    for n in 2..4
      a = [m, n]
      ans << a
    end
  end == (1..3)
end

assert('BS Block 8') do
  (1..3).to_a == [1, 2, 3]
end

assert('BS Block 9') do
  (1..3).map{|e|
    e * 4
  } == [4, 8, 12]
end

assert('BS Block 10') do
  def m
    yield
  end
  def n
    yield
  end

  m{
    n{
      100
    }
  } == 100
end

assert('BS Block 11') do
  def m
    yield 1
  end

  m{|ib|
    m{|jb|
      i = 20
    }
  } == 20
end

assert('BS Block 12') do
  def m
    yield 1
  end

  m{|ib|
    m{|jb|
      ib = 20
      kb = 2
    }
  } == 2
end

assert('BS Block 13') do
  def iter1
    iter2{
      yield
    }
  end

  def iter2
    yield
  end

  iter1{
    jb = 2
    iter1{
      jb = 3
    }
    jb
  } == 3
end

assert('BS Block 14') do
  def iter1
    iter2{
      yield
    }
  end

  def iter2
    yield
  end

  iter1{
    jb = 2
    iter1{
      jb
    }
    jb
  } == 2
end

assert('BS Block 15') do
  def m
    yield 1
  end
  m{|ib|
    ib*2
  } == 2
end

assert('BS Block 16') do
  def m
    yield 12345, 67890
  end
  m{|ib,jb|
    ib*2+jb
  } == 92580
end

assert('BS Block 17') do
  def iter
    yield 10
  end

  a = nil
  [iter{|a|
    a
  }, a] == [10, nil]
end

assert('BS Block 18') do
  def iter
    yield 10
  end

  iter{|a|
    iter{|a|
      a + 1
    } + a
  } == 21
end

assert('BS Block 19') do
  def iter
    yield 10, 20, 30, 40
  end

  a = b = c = d = nil
  iter{|a, b, c, d|
    [a, b, c, d]
  } + [a, b, c, d] == [10, 20, 30, 40, nil, nil, nil, nil]
end

assert('BS Block 20') do
  def iter
    yield 10, 20, 30, 40
  end

  a = b = nil
  iter{|a, b, c, d|
    [a, b, c, d]
  } + [a, b] == [10, 20, 30, 40, nil, nil]
end

assert('BS Block 21') do
  def iter
    yield 1, 2
  end

  iter{|a, *b|
    [a, b]
  } == [1, [2]]
end

assert('BS Block 22') do
  def iter
    yield 1, 2
  end

  iter{|*a|
    [a]
  } == [[1, 2]]
end

assert('BS Block 23') do
  def iter
    yield 1, 2
  end

  iter{|a, b, *c|
    [a, b, c]
  } == [1, 2, []]
end

assert('BS Block 24') do
  def m
    yield
  end
  m{
    1
  } == 1
end

assert('BS Block 25') do
  def m
    yield 123
  end
  m{|ib|
    m{|jb|
      ib*jb
    }
  } == 15129
end

assert('BS Block 26') do
  def m a
    yield a
  end
  m(1){|ib|
    m(2){|jb|
      ib*jb
    }
  } == 2
end

assert('BS Block 27') do
  sum = 0
  3.times{|ib|
    2.times{|jb|
      sum += ib + jb
    }}
  sum == 9
end

assert('BS Block 28') do
  3.times{|bl|
    break 10
  } == 10
end

assert('BS Block 29') do
  def iter
    yield 1,2,3
  end

  iter{|i, j|
    [i, j]
  } == [1, 2]
end

assert('BS Block 30') do
  def iter
    yield 1
  end

  iter{|i, j|
    [i, j]
  } == [1, nil]
end

assert('BS Block [ruby-dev:31147]') do
  def m
    yield
  end
  m{|&b| b} == nil
end

assert('BS Block [ruby-dev:31160]') do
  def m()
    yield
  end
  m {|(v,(*))|} == nil
end

assert('BS Block 31') do
  def m()
    yield
  end
  m {|((*))|} == nil
end

assert('BS Block [ruby-dev:31440]') do
  def m
    yield [0]
  end
  m{|v, &b| v} == [0]
end

assert('BS Block 32') do
  r = false; 1.times{|&b| r = b}; r.class == NilClass
end

assert('BS Block [ruby-core:14395]') do
  class Controller
    def respond_to(&block)
      responder = Responder.new
      block.call(responder)
      responder.respond
    end
    def test_for_bug
      respond_to{|format|
        format.js{
          "in test"
          render{|obj|
            obj
          }
        }
      }
    end
    def render(&block)
      "in render"
    end
  end

  class Responder
    def method_missing(symbol, &block)
      "enter method_missing"
      @response = Proc.new{
        'in method missing'
        block.call
      }
      "leave method_missing"
    end
    def respond
      @response.call
    end
  end
  t = Controller.new
  t.test_for_bug
end

assert("BS Block 33") do
  module TestReturnFromNestedBlock
    def self.test
      1.times do
        1.times do
          return :ok
        end
      end
      :bad
    end
  end
  TestReturnFromNestedBlock.test == :ok
end

assert("BS Block 34") do
  module TestReturnFromNestedBlock_BSBlock34
    def self.test
      1.times do
        while true
          return :ok
        end
      end
      :bad
    end
  end
  TestReturnFromNestedBlock_BSBlock34.test == :ok
end

assert("BS Block 35") do
  module TestReturnFromNestedBlock_BSBlock35
    def self.test
      1.times do
        until false
          return :ok
        end
      end
      :bad
    end
  end
  TestReturnFromNestedBlock_BSBlock35.test == :ok
end
