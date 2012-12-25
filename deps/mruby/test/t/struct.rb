##
# Struct ISO Test

if Object.const_defined?(:Struct)
  assert('Struct', '15.2.18') do
    Struct.class == Class
  end

  assert('Struct superclass', '15.2.18.2') do
    Struct.superclass == Object
  end

  assert('Struct.new', '15.2.18.3.1') do
    c = Struct.new(:m1, :m2)
    c.superclass == Struct and
      c.members == [:m1,:m2]
  end

  # Check crash bug with Struc.new and no params.
  assert('Struct.new', '15.2.18.3.1') do
     c = Struct.new()
     c.superclass == Struct and c.members == []
  end

  assert('Struct#==', '15.2.18.4.1') do
    c = Struct.new(:m1, :m2)
    cc1 = c.new(1,2)
    cc2 = c.new(1,2)
    cc1 == cc2
  end

  assert('Struct#[]', '15.2.18.4.2') do
    c = Struct.new(:m1, :m2)
    cc = c.new(1,2)
    cc[:m1] == 1 and cc["m2"] == 2
  end

  assert('Struct#[]=', '15.2.18.4.3') do
    c = Struct.new(:m1, :m2)
    cc = c.new(1,2)
    cc[:m1] = 3
    cc[:m1] == 3
  end

  assert('Struct#each', '15.2.18.4.4') do
    c = Struct.new(:m1, :m2)
    cc = c.new(1,2)
    a = []
    cc.each{|x|
      a << x
    }
    a[0] == 1 and a[1] == 2
  end

  assert('Struct#each_pair', '15.2.18.4.5') do
    c = Struct.new(:m1, :m2)
    cc = c.new(1,2)
    a = []
    cc.each_pair{|k,v|
      a << [k,v]
    }
    a[0] == [:m1, 1] and a[1] == [:m2, 2]
  end

  assert('Struct#members', '15.2.18.4.6') do
    c = Struct.new(:m1, :m2)
    cc = c.new(1,2)
    cc.members == [:m1,:m2]
  end

  assert('Struct#select', '15.2.18.4.7') do
    c = Struct.new(:m1, :m2)
    cc = c.new(1,2)
    cc.select{|v| v % 2 == 0} == [2]
  end
end

