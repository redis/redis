#!/usr/bin/env python3
"""Extract structured failures from `gh run view --log-failed` output."""

import argparse
import json
import re


ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
TIMESTAMP = re.compile(r"^[0-9-]+T[0-9:.]+Z ?")
TEST_FAILURE = re.compile(r"\[err\]:\s*(.+\s+in\s+(?:\.?/)?tests/\S+\.tcl)\s*$")
WARNING = "!!! WARNING The following tests failed:"
FALLBACK_ERROR = re.compile(
    r"(\[TIMEOUT\]|Valgrind error|Sanitizer error|Can't start|"
    r"error:|ERROR:|FAILED:|fatal:|FATAL:|Process completed with exit code)"
)


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
            if test_failures:
                failures.extend(test_failures)
            else:
                failures.append(parse_fallback(job, lines))
            continue
        exception = parse_exception(job, lines)
        failures.append(exception or parse_fallback(job, lines))
    return failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("output")
    args = parser.parse_args()
    with open(args.output, "w", encoding="utf-8") as output:
        json.dump(parse(args.log), output, indent=2)
