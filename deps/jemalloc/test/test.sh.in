#!/bin/sh

case @abi@ in
  macho)
    export DYLD_FALLBACK_LIBRARY_PATH="@objroot@lib"
    ;;
  pecoff)
    export PATH="${PATH}:@objroot@lib"
    ;;
  *)
    ;;
esac

# Corresponds to test_status_t.
pass_code=0
skip_code=1
fail_code=2

pass_count=0
skip_count=0
fail_count=0
for t in $@; do
  if [ $pass_count -ne 0 -o $skip_count -ne 0 -o $fail_count != 0 ] ; then
    echo
  fi
  echo "=== ${t} ==="
  ${t}@exe@ @abs_srcroot@ @abs_objroot@
  result_code=$?
  case ${result_code} in
    ${pass_code})
      pass_count=$((pass_count+1))
      ;;
    ${skip_code})
      skip_count=$((skip_count+1))
      ;;
    ${fail_code})
      fail_count=$((fail_count+1))
      ;;
    *)
      echo "Test harness error" 1>&2
      exit 1
  esac
done

total_count=`expr ${pass_count} + ${skip_count} + ${fail_count}`
echo
echo "Test suite summary: pass: ${pass_count}/${total_count}, skip: ${skip_count}/${total_count}, fail: ${fail_count}/${total_count}"

if [ ${fail_count} -eq 0 ] ; then
  exit 0
else
  exit 1
fi
