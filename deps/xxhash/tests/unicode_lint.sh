#!/bin/bash

# `unicode_lint.sh' determines whether source files under ${dirs} directories
# contain Unicode characters, and fails if any do.
#
# We don't recommend to call this script directly.
# Instead of it, use `make lint-unicode` via root directory Makefile.

# ${dirs} : target directories
dirs=(./ ./cli ./tests ./tests/bench ./tests/collisions)

SCRIPT_DIR="`dirname "${BASH_SOURCE[0]}"`"
cd ${SCRIPT_DIR}/..

echo "Ensure no unicode character is present in source files *.{c,h}"
pass=true

# Scan each directory in ${dirs} for Unicode in source (*.c, *.h) files
i=0
while [ $i -lt ${#dirs[@]} ]
do
  dir=${dirs[$i]}
  echo dir=$dir
  result=$(
    find ${dir} -regex '.*\.\(c\|h\)$' -exec grep -P -n "[^\x00-\x7F]" {} \; -exec echo "{}: FAIL" \;
  )
  if [[ $result ]]; then
    echo "$result"
    pass=false
  fi
  i=`expr $i + 1`
done


# Result
if [ "$pass" = true ]; then
  echo "All tests successful: no unicode character detected"
  echo "Result: PASS"
  exit 0
else
  echo "Result: FAIL"
  exit 1
fi
