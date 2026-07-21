#!/usr/bin/env python3
"""Render CI failure templates and create/update matching GitHub issues."""
import json, os, subprocess
from pathlib import Path

def gh(*args):
    return subprocess.run(["gh",*args],check=True,text=True,capture_output=True).stdout

def main():
    repo=os.environ["REPOSITORY"]; template=Path(os.environ["ISSUE_TEMPLATE"]).read_text()
    failures=json.loads(Path(os.environ["FAILURES_FILE"]).read_text())
    jobs={j["name"]:j["html_url"] for j in json.loads(Path(os.environ["JOBS_FILE"]).read_text())}
    issues=json.loads(gh("issue","list","--repo",repo,"--state","open","--label","ci-failure","--limit","1000","--json","number,title"))
    by_title={i["title"]:i["number"] for i in issues}
    for failure in failures:
        job=failure["job"]; job_url=jobs.get(job,os.environ["RUN_URL"])
        if failure["kind"] == "test":
            test_name,test_file=failure["test_case"].rsplit(" in ",1); title=f"[TEST] {test_name}"
        else:
            title=f"[TEST] {failure['title']}"; test_name,test_file=failure["title"],"N/A"
        body=template.format(test_name=test_name,test_file=test_file,ci_name=job,commit=os.environ["SHA"],job_url=job_url,error=failure["error"])
        number=by_title.get(title)
        if number: gh("issue","comment",str(number),"--repo",repo,"--body",f"### Failure recorded\n\n{body}")
        else:
            issue_url=gh("issue","create","--repo",repo,"--label","ci-failure","--title",title,"--body",body).strip()
            by_title[title]=int(issue_url.rsplit("/",1)[-1])
if __name__ == "__main__": main()
