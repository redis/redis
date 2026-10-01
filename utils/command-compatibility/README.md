# Advisory command compatibility checker

Compare the command metadata in a `redis/redis` pull request with its baseline.
The manual pilot helps reviewers find changes to the documented command contract;
it does **not** prove runtime compatibility or identify every breaking change.
It complements `utils/reply_schema_linter.js`, which validates current reply
schemas rather than comparing revisions.

## Run a comparison

After the workflow is merged into the default branch, a user with repository
write access can select **Redis command compatibility (manual pilot)** in Actions,
choose a trusted checker revision (normally `unstable`), and enter a PR number.
Alternatively:

```sh
gh workflow run redis-command-compatibility.yml --repo redis/redis --ref unstable -f pr_number=12345
```

Replace `12345` with the PR to inspect. The workflow runs offline tests before the
scanner and uses Python 3.12 with no third-party Python dependencies. It needs only
the built-in `GITHUB_TOKEN` with `contents: read`; no custom secret, cross-repository
dispatch, Redis build, or private infrastructure is required.

Contributors without workflow-dispatch permission can run the same utility locally
from a trusted checkout with Python 3.12:

```sh
python3 utils/command-compatibility/scan_redis_command_compatibility.py 12345 \
  --report /tmp/command-compatibility-report.json \
  --summary /tmp/command-compatibility-summary.md
```

The optional `GITHUB_TOKEN` environment variable authenticates public GitHub API
reads to avoid the lower anonymous rate limit. Do not put tokens in command-line
arguments or reports. The script reads only public command metadata; it never
builds or executes code from the inspected PR.

## Read the results

Open the run's job summary. The artifact
`redis-command-compatibility-<run_id>-<run_attempt>` contains
`command-compatibility-report.json` and is retained for 14 days. Public-repository
reports do not require access to private tooling; artifact downloads require
signing into GitHub. Reports describe the captured revisions, not the PR's live
state or outstanding bugs. A PR may have changed or merged since a report was made.

| Result | Meaning |
| --- | --- |
| `potential_breaking` | Metadata describes a potentially narrower contract, such as a removed command, option, or accepted argument count. Check intent and runtime behavior. |
| `review_required` | Flags, ACL categories, key specs, reply schemas, or ambiguous argument changes need interpretation. Additive reply fields can appear here; they are not confirmed bugs. |
| `completed` without findings | No reportable metadata changes were found. Runtime compatibility remains untested. |
| `partial` | Unsupported command definitions were excluded on both sides. Findings cover only the remaining commands; this is not a clean result. |
| `not_analyzed` | The revisions cannot be compared, for example because the head is already in the base history or is unavailable. No compatibility verdict was produced. |
| `incomplete` | Input, API, scanner, comparison, or reporting failed. This is not a clean compatibility result. |

Both completed outcomes exit zero, even with findings. The CLI exits 2 for `partial`
or `not_analyzed`; the manual workflow explicitly handles only that exit code as
an advisory notice and still publishes the report. Actual failures exit 1 and make
the workflow red. Report-delivery failures also fail the workflow, even after an
expected non-analysis result. This separate workflow is not a required merge check.
Always read the analysis status: a green workflow does not mean compatibility passed.

Each finding shows the command, field, old/new normalized values, explanation,
classification, PR link, and analyzed revisions. Metadata paths use JSON Pointer
notation (`~0` for `~`, `~1` for `/`); argument paths use dot/index notation.
`old_present` and `new_present` distinguish absent metadata from JSON `null`.
Review whether the change is compatible, intentional, or needs investigation.
An intentional breaking fix is still reportable and needs no bypass label.

Schema-version 2 JSON reports have `has_findings` and `finding_counts` (total,
potential_breaking, review_required) only for completed comparisons. Those fields
are `null` for every other status. Partial reports retain findings and provide
`partial_finding_counts`, `unanalyzable_commands` (side, identity, file, reason),
and `comparison_scope` (compared and excluded identities). Consumers must check
`status` before interpreting findings. The summary displays
at most 50 findings and shortens large values; completed JSON retains all findings.

## Revisions and historical PRs

The scanner resolves the PR's base/head SHAs once, then compares the merge base of
those SHAs with the PR head. This excludes unrelated target-branch changes.
Reports record the resolution time, exact before/after SHAs, source repositories,
checker version and workflow revision, file SHA-256 hashes, and command inventories.
Local runs have no checker revision unless `GITHUB_SHA` is set to the trusted
checker's commit. Re-running a PR number may resolve different revisions.

Fork head archives are fetched from the reported fork at the captured SHA. Only
an HTTP 404 from a distinct fork triggers one fallback to `redis/redis` at that
same SHA; the report records the source actually used. A missing head repository,
unavailable revision, or head already contained in its base history is `not_analyzed`,
not a zero-findings result. Some merged PRs remain comparable, but historical
baseline reconstruction is not supported. For example, do not treat an old
successful report as a guarantee that the same PR can still be scanned today.

Historical argument modifiers `optional`, `multiple`, and `multiple_token` may
use the exact string `"true"`; the checker normalizes it to boolean `true`, matching
its historical truthy interpretation by the command generator. Other non-boolean
values are unsupported, including `"false"` (a truthy string in the generator, not
a safe representation of false). The affected command is excluded from comparison
on both sides, with an explicit partial result. Original file hashes remain in
the report. An unsupported baseline command does not prevent unrelated commands
from being compared. An excluded command is never treated as a removed command.

## Scope and safety

The checker inspects `src/commands/*.json`. It compares command/subcommand identity,
arity, argument requiredness, repeatability, tokens, and choices. It ignores prose,
formatting, object-key order, and compatible command/option additions. Simple
regrouping and argument names are normalized; ambiguous syntax changes need review.
Optional-token reordering is review-only; independently narrowed syntax is still
reported. Flags, ACLs, key specs and reply schemas are review signals, not runtime
proof. Changes only in C code, performance, persistence formats, and module ABI are
outside scope and require targeted tests.

Only trusted checker code is executed. Archives are streamed without extraction;
only bounded regular command JSON files are parsed. Unsupported contract shapes
are isolated only after all command identities have been resolved. Duplicate JSON
keys, non-finite numbers, ambiguous/duplicate identities, malformed JSON, links and
corrupt archives still fail the entire scan; partial analysis cannot safely infer
the missing definitions. Credentials
are sent only to the GitHub API, never to public archive downloads or redirects.
Untrusted report text is escaped so it cannot inject summary links or markup.

Resource limits are 64 MiB per compressed archive, 256 MiB expanded, 2 MiB per JSON
file, 32 MiB of command JSON per snapshot, 4096 files, 1000 findings, 8 MiB of finding
data, and 16 MiB per report. A shared 100,000-unit comparison-work budget charges
candidate pairs, recursive probes, metadata traversal and encoded-data KiB. It
also charges the sequence aligner's worst-case pair work before entering it.
Probes consume work even when they produce no findings. Exhaustion writes an
incomplete report instead of waiting for the job timeout. These are conservative
work units, not a wall-clock guarantee. Exceeding a limit is incomplete analysis,
not success.
When output limits omit evidence, `truncated` and `truncated_fields` identify the
loss. File/command inventories are retained when possible. Report and summary
recovery are independent; failures must never masquerade as zero findings.

## Development and validation

Run focused offline tests (no Redis build or network required):

```sh
python3 -m unittest discover -s utils/command-compatibility -p 'test*redis_command_compatibility.py' -v
```

The separate `Command compatibility checker validation` workflow runs these tests,
Black and mypy on pushes/PRs that change this utility or its workflows. It does not
perform live PR compatibility scans, use custom secrets, or change branch protection.
Validation tools are pinned in `requirements-dev.txt`; scanner runtime remains
standard-library-only. To run the same checks locally in a Python 3.12 virtual environment:

```sh
python3 -m pip install -r utils/command-compatibility/requirements-dev.txt
python3 -m black --check --config utils/command-compatibility/pyproject.toml utils/command-compatibility
python3 -m mypy --config-file utils/command-compatibility/pyproject.toml utils/command-compatibility
```

The fixtures cover compatible additions, contract narrowing, and ambiguous
metadata changes. Tests also cover immutable revisions, fork retrieval, malformed
archives/JSON, historical modifiers, resource limits, and failure reporting.
Regression cases include partial baselines, exclusions across file moves and
nested subcommands, exhausted comparison work, expected unavailable revisions,
and workflow exit-code handling without swallowing real failures.
For manual evaluation, inspect a compatible-addition PR, a known syntax-narrowing
PR, and a PR without command metadata changes. Confirm exact revisions and expected
findings rather than treating a green workflow as proof of compatibility.

Automatic PR scans, notifications, required checks and broader runtime tests are
separate follow-ups after developers validate the usefulness and noise level.
