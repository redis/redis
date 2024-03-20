#!/usr/bin/env bash

TESTS=("$@")
RESULT_FILE=$(mktemp)
./runtests.sh "${TESTS[@]}" > "$RESULT_FILE" 2>&1
RET="$?"
if [ "${RET}" -ne 0 ]; then
    cat "$RESULT_FILE"
fi
rm "$RESULT_FILE"
exit $RET
