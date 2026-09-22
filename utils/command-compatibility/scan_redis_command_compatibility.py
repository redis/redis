#!/usr/bin/env python3
"""Read immutable Redis PR command snapshots without executing PR code."""

import argparse
import gzip
import hashlib
import html
import io
import json
import math
import os
import re
import sys
import tarfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, BinaryIO, cast
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, Request, build_opener

# Run as `python3 utils/command-compatibility/scan_redis_command_compatibility.py` or import
# from this directory (as unittest discovery does). This is not a package.
from check_redis_command_compatibility import FindingLimitError, compare_commands, normalize_snapshot

CHECKER_VERSION = "1.4.0-manual-pilot"
REPOSITORY = "redis/redis"
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
MAX_EXPANDED_BYTES = 256 * 1024 * 1024
MAX_JSON_BYTES = 2 * 1024 * 1024
MAX_METADATA_BYTES = 32 * 1024 * 1024
MAX_FILES = 4096
MAX_FINDINGS = 1000
MAX_REPORT_BYTES = 16 * 1024 * 1024
# Reserve half the report budget for inventories and other report metadata.
MAX_FINDING_BYTES = MAX_REPORT_BYTES // 2
MAX_SUMMARY_BYTES = 900 * 1024  # Leave room below GitHub's 1 MiB step-summary limit.


class ScanError(Exception):
    def __init__(self, kind: str, message: str, http_status: int | None = None) -> None:
        super().__init__(message)
        self.kind = kind
        self.http_status = http_status


class NoRedirect(HTTPRedirectHandler):
    # Never forward credentials to a URL supplied by a remote response.
    def redirect_request(self, req: Any, fp: Any, code: Any, msg: Any, headers: Any, newurl: Any) -> None:
        return None


def parse_json(data: bytes) -> Any:
    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Duplicate JSON object key")
            result[key] = value
        return result

    def reject_constant(value: str) -> None:
        raise ValueError("Non-finite JSON number")

    def finite_float(value: str) -> float:
        result = float(value)
        if not math.isfinite(result):
            raise ValueError("Non-finite JSON number")
        return result

    return json.loads(data, object_pairs_hook=unique_object, parse_constant=reject_constant, parse_float=finite_float)


def sha(value: Any) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ScanError("api", "GitHub returned a missing or invalid commit SHA")
    return value


def repository_name(value: Any) -> str:
    if (
        not isinstance(value, str)
        or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]{0,38}/[A-Za-z0-9_.-]{1,100}", value)
        or value.split("/")[-1] in {".", ".."}
    ):
        raise ScanError("api", "GitHub returned a missing or invalid head repository name")
    return value


class LimitedReader(io.RawIOBase):
    """Count all expanded bytes, including skipped members and tar padding."""

    def __init__(self, source: gzip.GzipFile, limit: int) -> None:
        self.source = source
        self.remaining = limit

    def read(self, size: int = -1) -> bytes:
        # tarfile's streaming reader requests bounded chunks. Never materialize
        # the expanded archive even if a caller accidentally requests read-all.
        if size < 0:
            raise ValueError("Expanded archive reads must have an explicit size")
        data = self.source.read(min(size, self.remaining + 1))
        self.remaining -= len(data)
        if self.remaining < 0:
            raise ValueError("Archive exceeds the expanded size limit")
        return data


class GitHub:
    def __init__(self, token: str = "") -> None:
        self.token = token

    def download(self, url: str, limit: int, authenticated: bool = False) -> bytes:
        headers = {"User-Agent": "redis-command-compatibility"}
        if authenticated:
            headers.update({"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28"})
            if self.token:
                headers["Authorization"] = f"Bearer {self.token}"
        try:
            with build_opener(NoRedirect()).open(Request(url, headers=headers), timeout=30) as response:
                data = response.read(limit + 1)
        except HTTPError as error:
            raise ScanError(
                "api", f"GitHub HTTP {error.code}; check PR access, rate limits and revision availability", error.code
            ) from error
        except (URLError, TimeoutError, OSError) as error:
            raise ScanError("api", "GitHub download failed or timed out; retry the manual run") from error
        if len(data) > limit:
            raise ScanError("api", "GitHub response exceeded the pilot download size limit")
        return data

    def api(self, path: str) -> dict[str, Any]:
        data = self.download(f"https://api.github.com/repos/{REPOSITORY}/{path}", 4 * 1024 * 1024, True)
        try:
            result = parse_json(data)
        except (ValueError, UnicodeError, RecursionError) as error:
            raise ScanError("api", "GitHub returned malformed JSON") from error
        if not isinstance(result, dict):
            raise ScanError("api", "GitHub returned an unexpected response shape")
        return result

    def snapshot(self, revision: str, repository: str = REPOSITORY) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        # Codeload is public: the GitHub token is sent only to api.github.com.
        repository = repository_name(repository)
        revision = sha(revision)
        try:
            data = self.download(f"https://codeload.github.com/{repository}/tar.gz/{revision}", MAX_ARCHIVE_BYTES)
        except ScanError as error:
            raise ScanError(
                error.kind, f"Cannot fetch {repository} at {revision}: {error}", error.http_status
            ) from error
        return read_snapshot(data, revision, repository)


def read_snapshot(
    data: bytes, revision: str, repository: str = REPOSITORY
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Stream regular command JSON members. Never extract an archive to disk."""
    files: dict[str, Any] = {}
    manifest: list[dict[str, Any]] = []
    archive_name = repository_name(repository).split("/")[1]
    prefix = f"{archive_name}-{sha(revision)}/src/commands/"
    total = 0
    try:
        with gzip.GzipFile(fileobj=io.BytesIO(data)) as compressed:
            expanded = LimitedReader(compressed, MAX_EXPANDED_BYTES)
            # The streaming tar interface only needs read(size); typeshed
            # describes it using the broader BinaryIO interface.
            with tarfile.open(fileobj=cast(BinaryIO, expanded), mode="r|") as archive:
                for member in archive:
                    # Python 3.12 caches headers even in streaming mode. We
                    # read by explicit TarInfo, so no member lookup is needed.
                    # The cache attribute is absent from tarfile's type stubs.
                    archive.members.clear()  # type: ignore[attr-defined]
                    if not member.name.startswith(prefix):
                        continue
                    name = member.name[len(prefix) :]
                    if "/" in name or not name.endswith(".json"):
                        continue
                    path = f"src/commands/{name}"
                    if not member.isfile() or not 0 <= member.size <= MAX_JSON_BYTES:
                        raise ValueError(f"{path}: command JSON must be a bounded regular file")
                    if path in files:
                        raise ValueError(f"{path}: duplicate command file in archive")
                    if len(files) >= MAX_FILES:
                        raise ValueError(f"Command file count exceeds the limit of {MAX_FILES}")
                    total += member.size
                    if total > MAX_METADATA_BYTES:
                        raise ValueError("Command metadata exceeds the total size limit")
                    stream = archive.extractfile(member)
                    if stream is None:
                        raise ValueError(f"{path}: unable to read command JSON")
                    with stream:
                        content = stream.read(MAX_JSON_BYTES + 1)
                    if len(content) != member.size:
                        raise ValueError(f"{path}: truncated command JSON")
                    try:
                        files[path] = parse_json(content)
                    except (ValueError, UnicodeError, RecursionError) as error:
                        raise ValueError(f"{path}: invalid JSON ({error})") from error
                    manifest.append(
                        {"path": path, "sha256": hashlib.sha256(content).hexdigest(), "bytes": len(content)}
                    )
            # tarfile stops at its end marker. Drain the gzip stream to check
            # the CRC/trailer and enforce the expansion limit on trailing data.
            while expanded.read(64 * 1024):
                pass
        if not files:
            raise ValueError("Snapshot has no src/commands/*.json files")
        commands = normalize_snapshot(files)
        if not commands:
            raise ValueError("Snapshot has no command definitions")
    except (OSError, EOFError, tarfile.TarError, ValueError, UnicodeError, RecursionError) as error:
        raise ScanError("scanner", f"Invalid command snapshot {repository} at {revision}: {error}") from error
    return commands, sorted(manifest, key=lambda item: item["path"])


def scan(pr_number: str, github: GitHub, report: dict[str, Any]) -> None:
    if not re.fullmatch(r"[1-9][0-9]{0,9}", pr_number):
        raise ScanError("input", "Provide a positive redis/redis PR number (digits only, up to 10 digits)")
    number = int(pr_number)
    report["pr"] = {"number": number, "url": f"https://github.com/{REPOSITORY}/pull/{number}"}
    pr = github.api(f"pulls/{number}")
    try:
        if pr["number"] != number or pr["base"]["repo"]["full_name"] != REPOSITORY:
            raise ScanError("api", "GitHub response does not identify the requested redis/redis PR")
        base = sha(pr["base"]["sha"])
        head = sha(pr["head"]["sha"])
        report["revisions"] = {
            "pr_base_sha": base,
            "pr_head_sha": head,
            "resolved_at": datetime.now(timezone.utc).isoformat(),
        }
        if pr["head"]["repo"] is None:
            raise ScanError(
                "unanalyzable", "PR head repository is unavailable (possibly deleted); cannot read its snapshot"
            )
        head_repository = repository_name(pr["head"]["repo"]["full_name"])
        report["pr"].update({"base_repository": REPOSITORY, "head_repository": head_repository})
        # GitHub's compare API accepts immutable SHAs within a fork network.
        # Do not substitute mutable head branch names for these captured SHAs.
        comparison = github.api(f"compare/{base}...{head}?per_page=1")
        before = sha(comparison["merge_base_commit"]["sha"])
    except (KeyError, TypeError) as error:
        raise ScanError("api", "GitHub PR/comparison response is missing required revision metadata") from error
    report["revisions"].update({"before_sha": before, "after_sha": head, "method": "merge_base_to_pr_head"})
    # An already-integrated head cannot be attributed using the current base.
    # Do not turn a merged/unavailable comparison into a misleading clean scan.
    if before == head:
        raise ScanError(
            "unanalyzable", "PR head is already in its base history; this comparison cannot isolate the PR changes"
        )
    before_commands, before_files = github.snapshot(before, REPOSITORY)
    report["snapshots"]["before"] = {
        "repository": REPOSITORY,
        "files": before_files,
        "commands": sorted(before_commands),
    }
    after_repository = head_repository
    snapshot_source = "pr_head_repository"
    try:
        after_commands, after_files = github.snapshot(head, after_repository)
    except ScanError as error:
        if head_repository == REPOSITORY or error.kind != "api" or error.http_status != 404:
            raise
        # Best effort only: preserve the captured SHA, never follow a mutable
        # pull ref or assume GitHub retains every former PR head indefinitely.
        after_repository = REPOSITORY
        snapshot_source = "base_repository_fallback_after_fork_404"
        try:
            after_commands, after_files = github.snapshot(head, after_repository)
        except ScanError as fallback_error:
            raise ScanError(
                fallback_error.kind,
                f"Fork archive {head_repository} at {head} returned HTTP 404; "
                f"fallback to {REPOSITORY} failed: {fallback_error}",
                fallback_error.http_status,
            ) from fallback_error
    report["snapshots"]["after"] = {
        "repository": after_repository,
        "snapshot_source": snapshot_source,
        "files": after_files,
        "commands": sorted(after_commands),
    }
    findings = compare_commands(
        before_commands, after_commands, max_findings=MAX_FINDINGS, max_finding_bytes=MAX_FINDING_BYTES
    )
    finding_bytes = 2  # JSON array brackets; count attribution and commas too.
    encoder = json.JSONEncoder(ensure_ascii=True, allow_nan=False, separators=(",", ":"))
    for index, finding in enumerate(findings):
        finding.update({"pr_url": report["pr"]["url"], "revisions": dict(report["revisions"])})
        finding_bytes += int(index > 0)
        for chunk in encoder.iterencode(finding):
            finding_bytes += len(chunk)
            if finding_bytes > MAX_FINDING_BYTES:
                raise FindingLimitError(MAX_FINDINGS, max_finding_bytes=MAX_FINDING_BYTES)
    report["findings"] = findings
    report["finding_counts"] = finding_counts(findings)
    report["has_findings"] = bool(findings)
    report["status"] = "completed"


def finding_counts(findings: list[dict[str, Any]]) -> dict[str, int]:
    return {
        "total": len(findings),
        "potential_breaking": sum(item["classification"] == "potential_breaking" for item in findings),
        "review_required": sum(item["classification"] == "review_required" for item in findings),
    }


def render_summary(report: dict[str, Any]) -> str:
    def text(value: Any, limit: int = 1200) -> str:
        rendered = json.dumps(value, ensure_ascii=True, sort_keys=True)
        if len(rendered) > limit:
            rendered = rendered[:limit] + " ... (see JSON report)"
        # Encode Markdown punctuation as well as HTML; untrusted data cannot add links/images.
        return "<code>" + "".join(f"&#{ord(char)};" for char in rendered) + "</code>"

    lines = ["# Redis command compatibility — manual pilot", "", f"Analysis status: **{report['status']}**", ""]
    if report.get("pr"):
        lines.append(f"PR: {report['pr']['url']}")
        if "head_repository" in report["pr"]:
            lines.append(f"Head repository: {text(report['pr']['head_repository'])}")
    lines += [
        "",
        "Advisory metadata analysis; findings require human review and do not prove a runtime regression.",
        "",
    ]
    for key, value in report.get("revisions", {}).items():
        lines.append(f"- {key}: {text(value)}")
    for side, snapshot in report["snapshots"].items():
        file_count = len(snapshot["files"]) if "files" in snapshot else snapshot["file_count"]
        command_count = len(snapshot["commands"]) if "commands" in snapshot else snapshot["command_count"]
        lines.append(f"- {side}: {file_count} JSON files, {command_count} commands/subcommands")
        if snapshot.get("snapshot_source") == "base_repository_fallback_after_fork_404":
            lines.append(
                "- Head archive source: redis/redis at the captured SHA after the fork archive returned HTTP 404."
            )
    if report["status"] != "completed":
        lines += ["", "**Incomplete analysis — do not interpret this as no compatibility findings.**"]
        if report.get("truncated"):
            lines.append(
                "**Evidence omitted.** See the errors and truncated_fields; this is not a complete comparison."
            )
        for error in report["errors"]:
            lines.append(f"- {text(error['kind'])}: {text(error['message'])}")
    else:
        counts = finding_counts(report["findings"])
        lines += [
            "",
            f"Findings: **{counts['total']}** — {counts['potential_breaking']} potential breaking; "
            f"{counts['review_required']} require review.",
        ]
        if report["findings"]:
            lines.append("**Review needed.** Analysis completed successfully; the findings below are advisory.")
        if not report["findings"]:
            lines.append(
                "No command-contract changes detected by this metadata checker; runtime compatibility is untested."
            )
        shown = 0
        for finding in report["findings"][:50]:
            block = [
                "",
                f"### {html.escape(finding['classification'])}",
                f"Command: {text(finding['command'])}; field: {text(finding['field'])}",
                "",
                f"Old: {text(finding['old'])}",
                "",
                f"New: {text(finding['new'])}",
                "",
                text(finding["explanation"]),
            ]
            if len("\n".join(lines + block).encode("utf-8")) > MAX_SUMMARY_BYTES - 4096:
                break
            lines += block
            shown += 1
        if shown < len(report["findings"]):
            lines += [
                "",
                f"Summary shows {shown} of {len(report['findings'])} findings (count/size limit). "
                "Download the JSON report for every finding and full values.",
            ]
    lines += [
        "",
        (
            "The JSON artifact records omitted output in truncated_fields."
            if report.get("truncated")
            else "The JSON artifact includes the checker version, analyzed revisions and complete file/command inventories."
        ),
        "",
    ]
    return "\n".join(lines)


class ReportLimitError(ValueError):
    pass


def bounded_report_json(report: dict[str, Any]) -> str:
    # ensure_ascii means each emitted character is exactly one UTF-8 byte.
    # Count chunks before retaining them, including the final newline.
    size = 1
    with io.StringIO() as output:
        for chunk in json.JSONEncoder(ensure_ascii=True, allow_nan=False, separators=(",", ":")).iterencode(report):
            size += len(chunk)
            if size > MAX_REPORT_BYTES:
                raise ReportLimitError(f"JSON report exceeds the {MAX_REPORT_BYTES}-byte pilot limit")
            output.write(chunk)
        output.write("\n")
        return output.getvalue()


def record_omission(report: dict[str, Any], field: str) -> None:
    report["truncated"] = True
    fields = report.setdefault("truncated_fields", [])
    if field not in fields:
        fields.append(field)


def serialize_report(report: dict[str, Any]) -> str:
    try:
        return bounded_report_json(report)
    except ReportLimitError as error:
        report.update(status="incomplete", has_findings=None, finding_counts=None)
        report["errors"].append({"kind": "reporting", "message": str(error)})
        # Drop findings first. File paths/hashes remain useful provenance and
        # usually fit comfortably once the larger finding evidence is omitted.
        if report["findings"]:
            report["findings"] = []
            record_omission(report, "findings")
        try:
            return bounded_report_json(report)
        except ReportLimitError:
            pass
        # An error can itself contain an oversized untrusted path. Shorten it
        # before considering any loss of file/command inventories.
        for item in report["errors"]:
            if len(item["message"]) > 1200:
                item["message"] = item["message"][:1200] + " ... (message truncated)"
                record_omission(report, "error_messages")
        try:
            return bounded_report_json(report)
        except ReportLimitError:
            pass
        for side, snapshot in report["snapshots"].items():
            for field, count_field in (("files", "file_count"), ("commands", "command_count")):
                if field in snapshot:
                    snapshot[count_field] = len(snapshot.pop(field))
                    record_omission(report, f"snapshots.{side}.{field}")
        return bounded_report_json(report)


def emergency_report(error: Exception) -> dict[str, Any]:
    # A trusted, minimal JSON object independent of the data/encoder path that
    # failed. Never emit NaN or remote exception contents in last-resort output.
    return {
        "schema_version": 1,
        "checker_version": CHECKER_VERSION,
        "repository": REPOSITORY,
        "status": "incomplete",
        "revisions": {},
        "snapshots": {},
        "findings": [],
        "has_findings": None,
        "finding_counts": None,
        "truncated": True,
        "truncated_fields": ["original_report"],
        "errors": [
            {"kind": "reporting", "message": f"Report recovery failed: {type(error).__name__}; evidence omitted"}
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pr_number", nargs="?", default=os.environ.get("OSS_PR_NUMBER", ""))
    parser.add_argument("--report", type=Path, default=Path("command-compatibility-report.json"))
    parser.add_argument("--summary", type=Path, default=os.environ.get("GITHUB_STEP_SUMMARY"))
    args = parser.parse_args(argv)
    report: dict[str, Any] = {
        "schema_version": 1,
        "checker_version": CHECKER_VERSION,
        "checker_revision": os.environ.get("GITHUB_SHA"),
        "repository": REPOSITORY,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "status": "incomplete",
        "revisions": {},
        "snapshots": {},
        "findings": [],
        "has_findings": None,
        "finding_counts": None,
        "truncated": False,
        "truncated_fields": [],
        "errors": [],
    }
    try:
        scan(args.pr_number, GitHub(os.environ.get("GITHUB_TOKEN", "")), report)
    except FindingLimitError as error:
        report.update(status="incomplete", has_findings=None, finding_counts=None, truncated=True)
        report["truncated_fields"] = ["findings"]
        report["errors"].append({"kind": "scanner", "message": str(error)})
    except ScanError as error:
        report.update(status="incomplete", has_findings=None, finding_counts=None)
        report["errors"].append({"kind": error.kind, "message": str(error)})
    except Exception as error:
        report.update(status="incomplete", has_findings=None, finding_counts=None)
        report["errors"].append({"kind": "scanner", "message": f"Unexpected scanner failure: {type(error).__name__}"})
    try:
        report_json = serialize_report(report)
        if args.summary:
            args.summary.write_text(render_summary(report), encoding="utf-8")
        args.report.write_text(report_json, encoding="utf-8")
    except Exception as error:
        report["status"] = "incomplete"
        report["has_findings"] = None
        report["finding_counts"] = None
        report["errors"].append({"kind": "reporting", "message": f"Unable to publish report: {type(error).__name__}"})
        # Construct each output independently: a broken encoder must not prevent
        # summary recovery, and a broken summary must not prevent JSON recovery.
        for path, is_report in [(args.report, True), (args.summary, False)]:
            if path:
                try:
                    content = serialize_report(report) if is_report else render_summary(report)
                except Exception as recovery_error:
                    if is_report:
                        report = emergency_report(recovery_error)
                        content = json.dumps(report, ensure_ascii=True, allow_nan=False, separators=(",", ":")) + "\n"
                    else:
                        content = "# Redis command compatibility\n\n**Incomplete analysis.** Summary recovery failed; check the JSON report and failed workflow steps.\n"
                try:
                    path.write_text(content, encoding="utf-8")
                except Exception:
                    pass
    print(
        json.dumps(
            {
                "status": report["status"],
                "findings": len(report["findings"]) if report["status"] == "completed" else None,
                "truncated": report["truncated"],
                "errors": report["errors"],
            }
        )
    )
    return 0 if report["status"] == "completed" else 1


if __name__ == "__main__":
    sys.exit(main())
