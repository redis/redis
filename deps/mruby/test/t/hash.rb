##
# Hash ISO Test

assert('Hash', '15.2.13') do
  Hash.class == Class
end

assert('Hash superclass', '15.2.13.2') do
  Hash.superclass == Object
end

assert('Hash#==', '15.2.13.4.1') do
  ({ 'abc' => 'abc' } ==  { 'abc' => 'abc' }) and
    not ({ 'abc' => 'abc' } ==  { 'cba' => 'cba' })
end

assert('Hash#[]', '15.2.13.4.2') do
  a = { 'abc' => 'abc' }

  a['abc'] == 'abc'
end

assert('Hash#[]=', '15.2.13.4.3') do
  a = Hash.new
  a['abc'] = 'abc'

  a['abc'] == 'abc'
end

assert('Hash#clear', '15.2.13.4.4') do
  a = { 'abc' => 'abc' }
  a.clear

  a == { }
end

assert('Hash#default', '15.2.13.4.5') do
  a = Hash.new
  b = Hash.new('abc')
  c = Hash.new {|s,k| s[k] = k}

  a.default == nil and b.default == 'abc' and
    c.default == nil and c.default('abc') == 'abc'
end

assert('Hash#default=', '15.2.13.4.6') do
  a = { 'abc' => 'abc' }
  a.default = 'cba'

  a['abc'] == 'abc' and a['notexist'] == 'cba'
end

assert('Hash#default_proc', '15.2.13.4.7') do
  a = Hash.new
  b = Hash.new {|s,k| s[k] = k}

  a.default_proc == nil and b.default_proc.class == Proc
end

assert('Hash#delete', '15.2.13.4.8') do
  a = { 'abc' => 'abc' }
  b = { 'abc' => 'abc' }
  b_tmp_1 = false
  b_tmp_2 = false

  a.delete('abc')
  b.delete('abc') do |k|
    b_tmp_1 = true
  end
  b.delete('abc') do |k|
    b_tmp_2 = true
  end

  a.delete('cba') == nil and not a.has_key?('abc') and
    not b_tmp_1 and b_tmp_2
end

assert('Hash#each', '15.2.13.4.9') do
  a = { 'abc_key' => 'abc_value' }
  key = nil
  value = nil

  a.each  do |k,v|
    key = k
    value = v
  end

  key == 'abc_key' and value == 'abc_value'
end

assert('Hash#each_key', '15.2.13.4.10') do
  a = { 'abc_key' => 'abc_value' }
  key = nil

  a.each_key  do |k|
    key = k
  end

  key == 'abc_key'
end

assert('Hash#each_value', '15.2.13.4.11') do
  a = { 'abc_key' => 'abc_value' }
  value = nil

  a.each_value  do |v|
    value = v
  end

  value == 'abc_value'
end

assert('Hash#empty?', '15.2.13.4.12') do
  a = { 'abc_key' => 'abc_value' }
  b = Hash.new

  not a.empty? and b.empty?
end

assert('Hash#has_key?', '15.2.13.4.13') do
  a = { 'abc_key' => 'abc_value' }
  b = Hash.new

  a.has_key?('abc_key') and not b.has_key?('cba')
end

assert('Hash#has_value?', '15.2.13.4.14') do
  a = { 'abc_key' => 'abc_value' }
  b = Hash.new

  a.has_value?('abc_value') and not b.has_value?('cba')
end

assert('Hash#include?', '15.2.13.4.15') do
  a = { 'abc_key' => 'abc_value' }
  b = Hash.new

  a.include?('abc_key') and not b.include?('cba')
end

assert('Hash#initialize copy', '15.2.13.4.17') do
  a = { 'abc_key' => 'abc_value' }
  b = Hash.new.initialize_copy(a)

  b == { 'abc_key' => 'abc_value' }
end

assert('Hash#key?', '15.2.13.4.18') do
  a = { 'abc_key' => 'abc_value' }
  b = Hash.new

  a.key?('abc_key') and not b.key?('cba')
end

assert('Hash#keys', '15.2.13.4.19') do
  a = { 'abc_key' => 'abc_value' }

  a.keys == ['abc_key']
end

assert('Hash#length', '15.2.13.4.20') do
  a = { 'abc_key' => 'abc_value' }
  b = Hash.new

  a.length == 1 and b.length == 0
end

assert('Hash#member?', '15.2.13.4.21') do
  a = { 'abc_key' => 'abc_value' }
  b = Hash.new

  a.member?('abc_key') and not b.member?('cba')
end

assert('Hash#merge', '15.2.13.4.22') do
  a = { 'abc_key' => 'abc_value', 'cba_key' => 'cba_value' }
  b = { 'cba_key' => 'XXX',  'xyz_key' => 'xyz_value' }

  result_1 = a.merge b
  result_2 = a.merge(b) do |key, original, new|
    original
  end

  result_1 == {'abc_key' => 'abc_value', 'cba_key' => 'XXX',
               'xyz_key' => 'xyz_value' } and
  result_2 == {'abc_key' => 'abc_value', 'cba_key' => 'cba_value',
               'xyz_key' => 'xyz_value' }
end

assert('Hash#replace', '15.2.13.4.23') do
  a = { 'abc_key' => 'abc_value' }
  b = Hash.new.replace(a)

  b == { 'abc_key' => 'abc_value' }
end

assert('Hash#shift', '15.2.13.4.24') do
  a = { 'abc_key' => 'abc_value', 'cba_key' => 'cba_value' }
  b = a.shift

  a == { 'abc_key' => 'abc_value' } and
    b == [ 'cba_key', 'cba_value' ]
end

assert('Hash#size', '15.2.13.4.25') do
  a = { 'abc_key' => 'abc_value' }
  b = Hash.new

  a.size == 1 and b.size == 0
end

assert('Hash#store', '15.2.13.4.26') do
  a = Hash.new
  a.store('abc', 'abc')

  a['abc'] == 'abc'
end

assert('Hash#value?', '15.2.13.4.27') do
  a = { 'abc_key' => 'abc_value' }
  b = Hash.new

  a.value?('abc_value') and not b.value?('cba')
end

assert('Hash#values', '15.2.13.4.28') do
  a = { 'abc_key' => 'abc_value' }

  a.values == ['abc_value']
end

# Not ISO specified

assert('Hash#reject') do
  h = {:one => 1, :two => 2, :three => 3, :four => 4}
  ret = h.reject do |k,v|
    v % 2 == 0
  end
  ret == {:one => 1, :three => 3} and
    h == {:one => 1, :two => 2, :three => 3, :four => 4}
end

assert('Hash#reject!') do
  h = {:one => 1, :two => 2, :three => 3, :four => 4}
  ret = h.reject! do |k,v|
    v % 2 == 0
  end
  ret == {:one => 1, :three => 3} and
    h == {:one => 1, :three => 3}
end

assert('Hash#select') do
  h = {:one => 1, :two => 2, :three => 3, :four => 4}
  ret = h.select do |k,v|
    v % 2 == 0
  end
  ret == {:two => 2, :four => 4} and
    h == {:one => 1, :two => 2, :three => 3, :four => 4}
end

assert('Hash#select!') do
  h = {:one => 1, :two => 2, :three => 3, :four => 4}
  ret = h.select! do |k,v|
    v % 2 == 0
  end
  ret == {:two => 2, :four => 4} and
    h == {:two => 2, :four => 4}
end

