#!/usr/bin/env python3
"""Parse failed CI logs and create or update matching GitHub issues."""

import argparse
import json
import os
import re
import subprocess
from pathlib import Path


ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
TIMESTAMP = re.compile(r"^[0-9-]+T[0-9:.]+Z ?")
TEST_FAILURE = re.compile(r"\[err\]:\s*(.+\s+in\s+(?:\.?/)?tests/\S+\.tcl)\s*$")
WARNING = "!!! WARNING The following tests failed:"
FALLBACK_ERROR = re.compile(
    r"(\[TIMEOUT\]|Valgrind error|Sanitizer error|Can't start|"
    r"error:|ERROR:|FAILED:|fatal:|FATAL:|Process completed with exit code)"
)


def gh(*args):
    return subprocess.run(
        ["gh", *args], check=True, text=True, capture_output=True
    ).stdout


def read_jobs(path):
    jobs = {}
    with open(path, encoding="utf-8", errors="replace") as log:
        for raw in log:
            fields = ANSI.sub("", raw.rstrip("\n")).split("\t", 2)
            if len(fields) != 3:
                continue
            job, line = fields[0], TIMESTAMP.sub("", fields[2])
            jobs.setdefault(job, []).append(line)
    return jobs


def parse_test_failures(job, lines):
    start = next(index for index, line in enumerate(lines) if WARNING in line) + 1
    summary = lines[start:]
    failures = []
    index = 0
    while index < len(summary):
        match = TEST_FAILURE.search(summary[index])
        if not match:
            index += 1
            continue

        error = [summary[index]]
        index += 1
        while index < len(summary):
            line = summary[index]
            if TEST_FAILURE.search(line) or line.startswith("Cleanup:"):
                break
            error.append(line)
            index += 1

        failures.append(
            {
                "kind": "test",
                "job": job,
                "test_case": match.group(1),
                "error": "\n".join(error).rstrip(),
            }
        )
    return failures


def parse_exception(job, lines):
    for index, line in enumerate(lines):
        if "[exception]:" not in line:
            continue
        return {
            "kind": "exception",
            "job": job,
            "test_case": None,
            "title": line.replace("[exception]:", "exception:", 1),
            "error": "\n".join(lines[index:]).rstrip(),
        }
    return None


def parse_fallback(job, lines):
    summary = next((line for line in lines if FALLBACK_ERROR.search(line)), None)
    return {
        "kind": "fallback",
        "job": job,
        "test_case": None,
        "title": f"unidentified failure in {job}",
        "error": summary or "No test failure marker was found in the failed log.",
    }


def parse(path):
    failures = []
    for job, lines in read_jobs(path).items():
        if any(WARNING in line for line in lines):
            test_failures = parse_test_failures(job, lines)
            failures.extend(test_failures or [parse_fallback(job, lines)])
            continue
        exception = parse_exception(job, lines)
        failures.append(exception or parse_fallback(job, lines))
    return failures


def issue_content(failure, template, jobs, sha, run_url):
    job = failure["job"]
    job_url = jobs.get(job, run_url)
    if failure["kind"] == "test":
        test_name, test_file = failure["test_case"].rsplit(" in ", 1)
        title = f"[TEST] {test_name}"
    else:
        title = f"[TEST] {failure['title']}"
        test_name, test_file = failure["title"], "N/A"
    body = template.format(
        test_name=test_name,
        test_file=test_file,
        ci_name=job,
        commit=sha,
        job_url=job_url,
        error=failure["error"],
    )
    return title, body


def report(failures, jobs_path):
    repo = os.environ["REPOSITORY"]
    template = Path(os.environ["ISSUE_TEMPLATE"]).read_text(encoding="utf-8")
    jobs = {
        job["name"]: job["html_url"]
        for job in json.loads(Path(jobs_path).read_text(encoding="utf-8"))
    }
    issues = json.loads(
        gh(
            "issue",
            "list",
            "--repo",
            repo,
            "--state",
            "open",
            "--label",
            "ci-failure",
            "--limit",
            "1000",
            "--json",
            "number,title",
        )
    )
    by_title = {issue["title"]: issue["number"] for issue in issues}

    for failure in failures:
        title, body = issue_content(
            failure,
            template,
            jobs,
            os.environ["SHA"],
            os.environ["RUN_URL"],
        )
        number = by_title.get(title)
        if number:
            gh(
                "issue",
                "comment",
                str(number),
                "--repo",
                repo,
                "--body",
                f"### Failure recorded\n\n{body}",
            )
        else:
            issue_url = gh(
                "issue",
                "create",
                "--repo",
                repo,
                "--label",
                "ci-failure",
                "--title",
                title,
                "--body",
                body,
            ).strip()
            by_title[title] = int(issue_url.rsplit("/", 1)[-1])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("jobs")
    args = parser.parse_args()
    report(parse(args.log), args.jobs)


if __name__ == "__main__":
    main()
