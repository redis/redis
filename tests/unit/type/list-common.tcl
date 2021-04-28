# We need a value larger than list-max-listpack-size to make sure
# the list has the right encoding when it is swapped in again.
array set largevalue {}
set largevalue(listpack) "hello"
set largevalue(linkedlist) [string repeat "hello" 4]
