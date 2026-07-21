#!/usr/bin/env python3
"""Extract structured failures from `gh run view --log-failed` output."""
import argparse, json, re
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
TIMESTAMP = re.compile(r"^[0-9-]+T[0-9:.]+Z\s+")
TEST_FAILURE = re.compile(r"\[err\]:\s*(.+\s+in\s+(?:\.?/)?tests/\S+\.tcl)\s*$")

def log_lines(path):
    for raw in open(path, encoding="utf-8", errors="replace"):
        fields = ANSI.sub("", raw.rstrip("\n")).split("\t", 2)
        if len(fields) == 3: yield fields[0], TIMESTAMP.sub("", fields[2])

def parse(path):
    failures, pending, first_job, summary = [], None, "unknown job", None
    def finish(record):
        if record and record not in failures: failures.append(record)
    for job, line in log_lines(path):
        if first_job == "unknown job": first_job = job
        if summary is None and re.search(r"(\[exception\]:|error:|ERROR:|FAILED:|fatal:|FATAL:|Process completed with exit code)", line): summary = line
        match = TEST_FAILURE.search(line)
        if match:
            finish(pending); pending = {"kind":"test","job":job,"test_case":match.group(1),"error":[]}; continue
        if "[exception]:" in line:
            finish(pending); pending = {"kind":"exception","job":job,"test_case":None,"title":line.replace("[exception]:","exception:",1),"error":[line]}; continue
        if pending:
            if line.startswith("Cleanup:") or re.match(r"\[(?:ok|err|exception)\]:", line): finish(pending); pending = None
            elif pending["kind"] == "exception": pending["error"].append(line)
            elif not pending["error"] and line.strip(): pending["error"].append(line)
    finish(pending)
    if not failures: failures.append({"kind":"fallback","job":first_job,"test_case":None,"title":f"unidentified failure in {first_job}","error":[summary or "No test failure marker was found in the failed log."]})
    for failure in failures: failure["error"] = "\n".join(failure["error"])
    return failures

if __name__ == "__main__":
    parser = argparse.ArgumentParser(); parser.add_argument("log"); parser.add_argument("output"); args = parser.parse_args()
    with open(args.output,"w",encoding="utf-8") as output: json.dump(parse(args.log),output,indent=2)
