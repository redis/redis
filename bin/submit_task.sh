#!/bin/bash
# set env vars
. ./env.sh

cd "$(dirname "$(find . -type f -name xcal-task.conf | head -1)")"
cat xcal-task.conf

# TODO: We need to replace with dynamically setting env vars in CICD env
# import AWS credentials
# aws configure import --csv "file://default_credentials.csv"
# cat ~/.aws/credentials

# submit task based on task_id1_context_user_id.json
# read ./xcalagent contents: fileinfor.json, preprocess.tar.gz, source_code.zip
echo "[CMD] python3 submit_task.py $1"
# python3 ./submit_task.py $1

