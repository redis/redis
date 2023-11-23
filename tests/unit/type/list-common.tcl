# We need a value to make sure the list has the right encoding when it is inserted.
array set largevalue {}
set largevalue(listpack) "hello"
set largevalue(quicklist) [string repeat "x" 8192]
