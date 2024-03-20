#!/usr/bin/env bash

TESTS=("$@")
ITER=0

while true; do
	./runtests.sh "${TESTS[@]}"
	RET="$?"
	if [ "${RET}" -ne 0 ]; then
		echo "Tests failed at loop $ITER"
		break
	fi
	echo "Finished loop $ITER"
	((ITER++))
done

