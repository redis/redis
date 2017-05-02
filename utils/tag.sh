#!/bin/bash

set -e

branch=$1
tag=$2

# branch can obviously be a generic
# git ref, but branch is cleaner terminology
if [[ (-z $branch) || (-z $tag) ]]; then
    echo "$0 [source branch] [tag]"
    exit 1
fi

here=`dirname $0`

"$here/lint.sh" $branch

if [[ $? == 0 ]]; then
    echo "Tagging $branch as $tag"
    git tag -f "$tag"
fi
