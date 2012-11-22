##
# Exception ISO Test

assert('Exception', '15.2.22') do
  Exception.class == Class
end

assert('Exception superclass', '15.2.22.2') do
  Exception.superclass == Object
end

assert('Exception.exception', '15.2.22.4.1') do
  e = Exception.exception('a')

  e.class == Exception
end

assert('Exception#exception', '15.2.22.5.1') do
  e1 = Exception.exception()
  e2 = Exception.exception('b')

  e1.class == Exception and e2.class == Exception
end

assert('Exception#message', '15.2.22.5.2') do
  e = Exception.exception('a')

  e.message == 'a'
end

assert('Exception#to_s', '15.2.22.5.3') do
  e = Exception.exception('a')

  e.to_s == 'a'
end

assert('Exception.exception', '15.2.22.4.1') do
  e = Exception.exception()
  e.initialize('a')

  e.message == 'a'
end

assert('ScriptError', '15.2.37') do
  begin
    raise ScriptError.new
  rescue ScriptError
    true
  else
    false
  end
end

assert('SyntaxError', '15.2.38') do
  begin
    raise SyntaxError.new
  rescue SyntaxError
    true
  else
    false
  end
end

# Not ISO specified

assert('Exception 1') do
  begin
    1+1
  ensure
    2+2
  end == 2
end

assert('Exception 2') do
  begin
    1+1
    begin
      2+2
    ensure
      3+3
    end
  ensure
    4+4
  end == 4
end

assert('Exception 3') do
  begin
    1+1
    begin
      2+2
    ensure
      3+3
    end
  ensure
    4+4
    begin
      5+5
    ensure
      6+6
    end
  end == 4
end

assert('Exception 4') do
  a = nil
  1.times{|e|
    begin
    rescue => err
    end
    a = err.class
  }
  a == NilClass
end

assert('Exception 5') do
  $ans = []
  def m
    $!
  end
  def m2
    1.times{
      begin
        return
      ensure
        $ans << m
      end
    }
  end
  m2
  $ans == [nil]
end

assert('Exception 6') do
  $i = 0
  def m
    iter{
      begin
        $i += 1
        begin
          $i += 2
          break
        ensure

        end
      ensure
        $i += 4
      end
      $i = 0
    }
  end

  def iter
    yield
  end
  m
  $i == 7
end

assert('Exception 7') do
  $i = 0
  def m
    begin
      $i += 1
      begin
        $i += 2
        return
      ensure
        $i += 3
      end
    ensure
      $i += 4
    end
    p :end
  end
  m
  $i == 10
end

assert('Exception 8') do
  begin
    1
  rescue
    2
  else
    3
  end == 3
end

assert('Exception 9') do
  begin
    1+1
  rescue
    2+2
  else
    3+3
  ensure
    4+4
  end == 6
end

assert('Exception 10') do
  begin
    1+1
    begin
      2+2
    rescue
      3+3
    else
      4+4
    end
  rescue
    5+5
  else
    6+6
  ensure
    7+7
  end == 12
end

assert('Exception 11') do
  a = :ok
  begin
    begin
      raise Exception
    rescue
      a = :ng
    end
  rescue Exception
  end
  a == :ok
end

assert('Exception 12') do
  a = :ok
  begin
    raise Exception rescue a = :ng
  rescue Exception
  end
  a == :ok
end

assert('Exception 13') do
  a = :ng
  begin
    raise StandardError
  rescue TypeError, ArgumentError
    a = :ng
  rescue
    a = :ok
  else
    a = :ng
  end
  a == :ok
end

def exception_test14
  UnknownConstant
end

assert('Exception 14') do
  a = :ng
  begin
    send(:exception_test14)
  rescue
    a = :ok
  end

  a == :ok
end

assert('Exception 15') do
  a = begin
        :ok
      rescue
        :ng
      end
  a == :ok
end

assert('Exception 16') do
  begin
    raise "foo"
    false
  rescue => e
    e.message == "foo"
  end
end

assert('Exception#inspect without message') do
  Exception.new.inspect
end

# very deeply recursive function that stil returns albeit very deeply so
$test_infinite_recursion    = 0
TEST_INFINITE_RECURSION_MAX = 100000
def test_infinite_recursion
  $test_infinite_recursion += 1
  if $test_infinite_recursion > TEST_INFINITE_RECURSION_MAX
    return $test_infinite_recursion 
  end
  test_infinite_recursion 
end

assert('Infinite recursion should result in an exception being raised') do
    a = begin 
          test_infinite_recursion
        rescue 
          :ok
        end
    # OK if an exception was caught, otherwise a number will be stored in a
    a == :ok
end


