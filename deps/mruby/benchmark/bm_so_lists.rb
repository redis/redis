#from http://www.bagley.org/~doug/shootout/bench/lists/lists.ruby

NUM = 300
SIZE = 10000

def test_lists()
  # create a list of integers (Li1) from 1 to SIZE
  li1 = (1..SIZE).to_a
  # copy the list to li2 (not by individual items)
  li2 = li1.dup
  # remove each individual item from left side of li2 and
  # append to right side of li3 (preserving order)
  li3 = Array.new
  while (not li2.empty?)
    li3.push(li2.shift)
  end
  # li2 must now be empty
  # remove each individual item from right side of li3 and
  # append to right side of li2 (reversing list)
  while (not li3.empty?)
    li2.push(li3.pop)
  end
  # li3 must now be empty
  # reverse li1 in place
  li1.reverse!
  # check that first item is now SIZE
  if li1[0] != SIZE then
    p "not SIZE"
    0
  else
    # compare li1 and li2 for equality
    if li1 != li2 then
      return(0)
    else
      # return the length of the list
      li1.length
    end
  end
end

i = 0
while i<NUM
  i+=1
  result = test_lists()
end

result
