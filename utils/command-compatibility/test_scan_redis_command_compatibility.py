"""Offline tests for the trusted manual-pilot runner."""

import contextlib
import copy
import gzip
import hashlib
import io
import json
import tarfile
import tempfile
import unittest
from email.message import Message
from http.client import HTTPMessage
from pathlib import Path
from typing import Any
from unittest.mock import Mock, patch
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, Request

import scan_redis_command_compatibility as scanner

BASE = "a" * 40
BEFORE = "b" * 40
HEAD = "c" * 40
PR_NUMBER = 123
COMMANDS = {
    "GET": {
        "arity": 2,
        "arguments": [{"name": "key", "type": "key", "key_spec_index": 0}],
    },
    "PING": {"arity": -1, "arguments": [{"name": "message", "type": "string", "optional": True}]},
}


def command_bytes(commands: dict[str, Any] | None = None) -> bytes:
    return json.dumps(COMMANDS if commands is None else commands).encode()


def archive_bytes(
    revision: str = BEFORE,
    members: list[tuple[str, bytes, bytes]] | None = None,
    repository: str = "redis/redis",
) -> bytes:
    """Build synthetic archives in memory; members are (path, content, tar type)."""
    if members is None:
        members = [
            (f"{repository.split('/')[1]}-{revision}/src/commands/example.json", command_bytes(), tarfile.REGTYPE)
        ]
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w") as archive:
        for name, content, kind in members:
            member = tarfile.TarInfo(name)
            member.type = kind
            if kind in (tarfile.SYMTYPE, tarfile.LNKTYPE):
                member.linkname = "../../outside.json"
            else:
                member.size = len(content)
            archive.addfile(member, io.BytesIO(content) if member.isfile() else None)
    return gzip.compress(raw.getvalue())


def fresh_report() -> dict[str, Any]:
    return {"status": "incomplete", "revisions": {}, "snapshots": {}, "findings": [], "errors": []}


class FakeGitHub(scanner.GitHub):
    def __init__(self, after: dict[str, Any] | None = None) -> None:
        self.pr: dict[str, Any] = {
            "number": PR_NUMBER,
            "base": {"sha": BASE, "repo": {"full_name": "redis/redis"}},
            "head": {"sha": HEAD, "repo": {"full_name": "redis/redis"}},
            "merged": False,
        }
        self.comparison: dict[str, Any] = {"merge_base_commit": {"sha": BEFORE}}
        self.api_calls: list[str] = []
        self.snapshot_calls: list[tuple[str, str]] = []
        self.after = copy.deepcopy(COMMANDS if after is None else after)

    def api(self, path: str) -> dict[str, Any]:
        self.api_calls.append(path)
        if path == f"pulls/{PR_NUMBER}":
            return copy.deepcopy(self.pr)
        if path == f"compare/{self.pr['base']['sha']}...{self.pr['head']['sha']}?per_page=1":
            return copy.deepcopy(self.comparison)
        raise AssertionError(f"Unexpected API request: {path}")

    def snapshot(
        self, revision: str, repository: str = scanner.REPOSITORY
    ) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        self.snapshot_calls.append((revision, repository))
        if revision == BEFORE:
            commands = COMMANDS
        elif revision == HEAD:
            commands = self.after
        else:
            raise AssertionError("Only the immutable merge base and PR head may be downloaded")
        data = archive_bytes(
            revision,
            [
                (
                    f"{repository.split('/')[1]}-{revision}/src/commands/example.json",
                    command_bytes(commands),
                    tarfile.REGTYPE,
                )
            ],
        )
        return scanner.read_snapshot(data, revision, repository)


class RevisionTests(unittest.TestCase):
    def test_compares_merge_base_to_head_and_records_analyzed_revisions(self) -> None:
        github = FakeGitHub({"GET": COMMANDS["GET"]})
        report = fresh_report()
        scanner.scan(str(PR_NUMBER), github, report)
        self.assertEqual(report["status"], "completed")
        self.assertEqual(github.snapshot_calls, [(BEFORE, "redis/redis"), (HEAD, "redis/redis")])
        self.assertEqual(report["revisions"]["pr_base_sha"], BASE)
        self.assertEqual(report["revisions"]["before_sha"], BEFORE)
        self.assertEqual(report["revisions"]["after_sha"], HEAD)
        self.assertEqual(report["revisions"]["pr_head_sha"], HEAD)
        self.assertNotEqual(report["revisions"]["before_sha"], BASE)
        self.assertTrue(report["findings"])
        for finding in report["findings"]:
            self.assertEqual(finding["pr_url"], f"https://github.com/redis/redis/pull/{PR_NUMBER}")
            self.assertEqual(finding["revisions"], report["revisions"])
        self.assertEqual(report["snapshots"]["before"]["commands"], ["GET", "PING"])
        self.assertEqual(report["snapshots"]["after"]["commands"], ["GET"])

    def test_already_integrated_head_is_incomplete_instead_of_clean(self) -> None:
        for merged in (False, True):
            with self.subTest(merged=merged):
                github = FakeGitHub()
                github.pr["merged"] = merged
                github.comparison["merge_base_commit"]["sha"] = HEAD
                report = fresh_report()
                with self.assertRaises(scanner.ScanError) as caught:
                    scanner.scan(str(PR_NUMBER), github, report)
                self.assertEqual(caught.exception.kind, "unanalyzable")
                self.assertEqual(report["status"], "incomplete")
                self.assertEqual(github.snapshot_calls, [])

    def test_fork_head_uses_its_repository_at_the_pinned_sha(self) -> None:
        github = FakeGitHub()
        github.pr["head"]["repo"]["full_name"] = "contributor/redis-renamed"
        report = fresh_report()
        scanner.scan(str(PR_NUMBER), github, report)
        self.assertEqual(github.snapshot_calls, [(BEFORE, "redis/redis"), (HEAD, "contributor/redis-renamed")])
        self.assertEqual(report["pr"]["head_repository"], "contributor/redis-renamed")
        self.assertEqual(report["snapshots"]["after"]["repository"], "contributor/redis-renamed")
        self.assertIn(f"compare/{BASE}...{HEAD}?per_page=1", github.api_calls)

    def test_deleted_head_repository_is_unanalyzable_with_revision_context(self) -> None:
        github = FakeGitHub()
        github.pr["head"]["repo"] = None
        report = fresh_report()
        with self.assertRaises(scanner.ScanError) as caught:
            scanner.scan(str(PR_NUMBER), github, report)
        self.assertEqual(caught.exception.kind, "unanalyzable")
        self.assertIn("head repository", str(caught.exception))
        self.assertEqual(report["revisions"]["pr_head_sha"], HEAD)
        self.assertEqual(github.snapshot_calls, [])

    def test_fork_archive_404_falls_back_to_same_sha_and_records_actual_source(self) -> None:
        github = FakeGitHub()
        github.pr["head"]["repo"]["full_name"] = "contributor/redis-renamed"
        original = github.snapshot

        def snapshot(revision: str, repository: str) -> tuple[dict[str, Any], list[dict[str, Any]]]:
            if repository == "contributor/redis-renamed":
                raise scanner.ScanError("api", "GitHub HTTP 404", 404)
            return original(revision, repository)

        report = fresh_report()
        with patch.object(github, "snapshot", side_effect=snapshot) as calls:
            scanner.scan(str(PR_NUMBER), github, report)
        self.assertEqual(
            [call.args for call in calls.call_args_list],
            [(BEFORE, "redis/redis"), (HEAD, "contributor/redis-renamed"), (HEAD, "redis/redis")],
        )
        self.assertEqual(report["status"], "completed")
        self.assertEqual(report["pr"]["head_repository"], "contributor/redis-renamed")
        self.assertEqual(report["snapshots"]["after"]["repository"], "redis/redis")
        self.assertEqual(report["snapshots"]["after"]["snapshot_source"], "base_repository_fallback_after_fork_404")
        self.assertEqual(report["revisions"]["after_sha"], HEAD)
        self.assertIn("fork archive returned HTTP 404", scanner.render_summary(report))

    def test_snapshot_fallback_only_handles_distinct_fork_api_404(self) -> None:
        cases = [
            ("redis/redis", "api", 404),
            ("contributor/redis", "api", 403),
            ("contributor/redis", "api", 429),
            ("contributor/redis", "api", 500),
            ("contributor/redis", "api", 302),
            ("contributor/redis", "api", None),
            ("contributor/redis", "scanner", 404),
        ]
        for repository, kind, http_status in cases:
            with self.subTest(repository=repository, kind=kind, status=http_status):
                github = FakeGitHub()
                github.pr["head"]["repo"]["full_name"] = repository
                before = github.snapshot(BEFORE)
                failure = scanner.ScanError(kind, "Synthetic failure", http_status)
                with patch.object(github, "snapshot", side_effect=[before, failure]) as calls:
                    with self.assertRaises(scanner.ScanError) as caught:
                        scanner.scan(str(PR_NUMBER), github, fresh_report())
                self.assertIs(caught.exception, failure)
                self.assertEqual(calls.call_count, 2)

    def test_failed_fallback_keeps_both_repository_attempts_in_error(self) -> None:
        github = FakeGitHub()
        github.pr["head"]["repo"]["full_name"] = "contributor/redis"
        before = github.snapshot(BEFORE)
        report = fresh_report()
        with patch.object(
            github,
            "snapshot",
            side_effect=[
                before,
                scanner.ScanError("api", "GitHub HTTP 404", 404),
                scanner.ScanError("api", "GitHub HTTP 404", 404),
            ],
        ):
            with self.assertRaises(scanner.ScanError) as caught:
                scanner.scan(str(PR_NUMBER), github, report)
        self.assertEqual(caught.exception.kind, "api")
        self.assertEqual(caught.exception.http_status, 404)
        for expected in ("contributor/redis", "redis/redis", HEAD, "fallback"):
            self.assertIn(expected, str(caught.exception))
        self.assertEqual(report["status"], "incomplete")
        self.assertEqual(report["revisions"]["after_sha"], HEAD)
        self.assertTrue(report["snapshots"]["before"]["files"])

    def test_invalid_head_repository_names_are_not_used_in_urls(self) -> None:
        for name in ("https://evil.example/repo", "owner/..", "owner/repo?secret", "owner/a/b", "owner/repo#ref", None):
            with self.subTest(name=name):
                github = FakeGitHub()
                github.pr["head"]["repo"]["full_name"] = name
                with self.assertRaises(scanner.ScanError) as caught:
                    scanner.scan(str(PR_NUMBER), github, fresh_report())
                self.assertEqual(caught.exception.kind, "api")
                self.assertEqual(github.snapshot_calls, [])

    def test_rerun_records_new_base_resolution_but_keeps_comparison_attributed(self) -> None:
        github = FakeGitHub()
        first, second = fresh_report(), fresh_report()
        scanner.scan(str(PR_NUMBER), github, first)
        github.pr["base"]["sha"] = "d" * 40
        scanner.scan(str(PR_NUMBER), github, second)
        self.assertNotEqual(first["revisions"]["pr_base_sha"], second["revisions"]["pr_base_sha"])
        self.assertEqual(first["revisions"]["before_sha"], second["revisions"]["before_sha"])
        self.assertEqual(first["findings"], second["findings"])
        self.assertTrue(second["revisions"]["resolved_at"])

    def test_invalid_pr_inputs_do_not_access_github(self) -> None:
        for value in ("", "0", "-1", "1.5", " 123", "123\n", "01", "123;echo test", "12345678901"):
            with self.subTest(value=value):
                github = FakeGitHub()
                with self.assertRaises(scanner.ScanError) as caught:
                    scanner.scan(value, github, fresh_report())
                self.assertEqual(caught.exception.kind, "input")
                self.assertEqual(github.api_calls, [])

    def test_missing_or_inconsistent_api_metadata_fails_before_download(self) -> None:
        variants: list[dict[str, Any]] = [
            {},
            {"base": {}},
            {"number": PR_NUMBER + 1},
            {"base": {"sha": BASE, "repo": {"full_name": "other/repository"}}},
            {"head": {"sha": "unstable"}},
            {"head": None},
        ]
        for variant in variants:
            with self.subTest(variant=variant):
                github = FakeGitHub()
                github.pr = {**github.pr, **variant} if variant else {}
                with self.assertRaises(scanner.ScanError) as caught:
                    scanner.scan(str(PR_NUMBER), github, fresh_report())
                self.assertEqual(caught.exception.kind, "api")
                self.assertEqual(github.snapshot_calls, [])

    def test_invalid_comparison_metadata_is_an_api_error(self) -> None:
        for comparison in ({}, {"merge_base_commit": None}, {"merge_base_commit": {"sha": "bad"}}):
            with self.subTest(comparison=comparison):
                github = FakeGitHub()
                github.comparison = comparison
                with self.assertRaises(scanner.ScanError) as caught:
                    scanner.scan(str(PR_NUMBER), github, fresh_report())
                self.assertEqual(caught.exception.kind, "api")
                self.assertEqual(github.snapshot_calls, [])


class SnapshotTests(unittest.TestCase):
    def assert_invalid_snapshot(self, data: bytes) -> None:
        with self.assertRaises(scanner.ScanError) as caught:
            scanner.read_snapshot(data, BEFORE)
        self.assertEqual(caught.exception.kind, "scanner")

    def test_only_regular_direct_command_json_is_analyzed_without_extraction(self) -> None:
        content = command_bytes()
        data = archive_bytes(
            members=[
                (f"redis-{BEFORE}/src/commands/example.json", content, tarfile.REGTYPE),
                (f"redis-{BEFORE}/src/commands/nested/ignored.json", b"not json", tarfile.REGTYPE),
                (f"redis-{BEFORE}/src/commands/../outside.json", b"not json", tarfile.REGTYPE),
                (f"redis-{BEFORE}/src/commands/generate.py", b"raise RuntimeError('never execute')", tarfile.REGTYPE),
                ("../outside.json", b"not json", tarfile.REGTYPE),
            ]
        )
        with patch.object(tarfile.TarFile, "extractall", side_effect=AssertionError("Never extract OSS archives")):
            commands, manifest = scanner.read_snapshot(data, BEFORE)
        self.assertEqual(set(commands), {"GET", "PING"})
        self.assertEqual(
            manifest,
            [
                {
                    "path": "src/commands/example.json",
                    "sha256": hashlib.sha256(content).hexdigest(),
                    "bytes": len(content),
                }
            ],
        )

    def test_symlink_and_hardlink_command_files_are_rejected(self) -> None:
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE):
            with self.subTest(kind=kind):
                self.assert_invalid_snapshot(
                    archive_bytes(members=[(f"redis-{BEFORE}/src/commands/example.json", b"", kind)])
                )

    def test_duplicate_command_archive_members_are_rejected(self) -> None:
        member = (f"redis-{BEFORE}/src/commands/example.json", command_bytes(), tarfile.REGTYPE)
        self.assert_invalid_snapshot(archive_bytes(members=[member, member]))

    def test_empty_missing_or_wrong_revision_snapshots_are_rejected(self) -> None:
        cases = [
            archive_bytes(members=[]),
            archive_bytes(HEAD),
            archive_bytes(members=[(f"redis-{BEFORE}/src/commands/example.json", b"{}", tarfile.REGTYPE)]),
            b"not a gzip archive",
            gzip.compress(b"not a tar archive"),
            archive_bytes()[:30],
        ]
        for data in cases:
            with self.subTest(size=len(data)):
                self.assert_invalid_snapshot(data)

    def test_malformed_duplicate_and_nonfinite_json_are_rejected(self) -> None:
        for content in (
            b"not JSON",
            b'{"GET": {}, "GET": {}}',
            b'{"GET": {"arity": 2, "arity": 3}}',
            b'{"GET": {"arity": NaN}}',
            b'{"GET": {"arity": Infinity}}',
            b'{"GET": {"arity": 1e999}}',
            b"\xff",
        ):
            with self.subTest(content=content):
                self.assert_invalid_snapshot(
                    archive_bytes(members=[(f"redis-{BEFORE}/src/commands/example.json", content, tarfile.REGTYPE)])
                )

    def test_archive_and_command_metadata_limits_fail_explicitly(self) -> None:
        for name in ("MAX_EXPANDED_BYTES", "MAX_JSON_BYTES", "MAX_METADATA_BYTES", "MAX_FILES"):
            with self.subTest(limit=name), patch.object(scanner, name, 0):
                self.assert_invalid_snapshot(archive_bytes())

    def test_gzip_reads_stay_bounded_and_trailer_is_verified(self) -> None:
        original = gzip.GzipFile.read
        read_sizes: list[int] = []

        def read(source: gzip.GzipFile, size: int = -1) -> bytes:
            read_sizes.append(size)
            return original(source, size)

        with patch.object(gzip.GzipFile, "read", read):
            commands, _ = scanner.read_snapshot(archive_bytes(), BEFORE)
        self.assertEqual(set(commands), {"GET", "PING"})
        self.assertTrue(all(0 < size <= 64 * 1024 for size in read_sizes))
        data = archive_bytes()
        self.assert_invalid_snapshot(data[:-8])
        corrupt_crc = bytearray(data)
        corrupt_crc[-8] ^= 1
        self.assert_invalid_snapshot(bytes(corrupt_crc))

    def test_expanded_limit_includes_data_after_tar_end_marker(self) -> None:
        expanded = gzip.decompress(archive_bytes())
        data = gzip.compress(expanded + b"padding" * 1000)
        with patch.object(scanner, "MAX_EXPANDED_BYTES", len(expanded) + 100):
            self.assert_invalid_snapshot(data)

    def test_ignored_archive_members_do_not_accumulate_header_metadata(self) -> None:
        members = [(f"ignored/{index}/" + "x" * 120, b"", tarfile.REGTYPE) for index in range(200)]
        members.append((f"redis-{BEFORE}/src/commands/example.json", command_bytes(), tarfile.REGTYPE))
        data = archive_bytes(members=members)
        original = tarfile.TarFile.next
        cache_sizes: list[int] = []

        def next_member(archive: tarfile.TarFile) -> tarfile.TarInfo | None:
            result = original(archive)
            cache_sizes.append(len(archive.members))  # type: ignore[attr-defined]
            return result

        with patch.object(tarfile.TarFile, "next", next_member):
            commands, manifest = scanner.read_snapshot(data, BEFORE)
        self.assertEqual(set(commands), {"GET", "PING"})
        self.assertEqual(len(manifest), 1)
        self.assertGreater(len(cache_sizes), 200)
        self.assertLessEqual(max(cache_sizes), 1)

    def test_duplicate_and_file_count_errors_are_distinct(self) -> None:
        first = (f"redis-{BEFORE}/src/commands/first.json", command_bytes(), tarfile.REGTYPE)
        second = (f"redis-{BEFORE}/src/commands/second.json", command_bytes(), tarfile.REGTYPE)
        with self.assertRaisesRegex(scanner.ScanError, "first.json: duplicate command file"):
            scanner.read_snapshot(archive_bytes(members=[first, first]), BEFORE)
        with patch.object(scanner, "MAX_FILES", 1), self.assertRaisesRegex(scanner.ScanError, "file count exceeds"):
            scanner.read_snapshot(archive_bytes(members=[first, second]), BEFORE)

    def test_invalid_json_identifies_repository_revision_and_file(self) -> None:
        data = archive_bytes(members=[(f"copy-{BEFORE}/src/commands/bad.json", b"not json", tarfile.REGTYPE)])
        with self.assertRaises(scanner.ScanError) as caught:
            scanner.read_snapshot(data, BEFORE, "contributor/copy")
        message = str(caught.exception)
        for expected in ("contributor/copy", BEFORE, "src/commands/bad.json"):
            self.assertIn(expected, message)


class GitHubTests(unittest.TestCase):
    def test_snapshot_download_preserves_structured_http_status(self) -> None:
        opener = Mock()
        opener.open.side_effect = HTTPError("https://codeload.github.com/", 404, "secret details", Message(), None)
        with patch.object(scanner, "build_opener", return_value=opener):
            with self.assertRaises(scanner.ScanError) as caught:
                scanner.GitHub("secret-token").snapshot(HEAD, "contributor/redis")
        self.assertEqual(caught.exception.http_status, 404)
        self.assertEqual(caught.exception.kind, "api")
        self.assertNotIn("secret", str(caught.exception))
        self.assertIsNone(opener.open.call_args.args[0].get_header("Authorization"))

    def test_token_is_sent_only_to_github_api_and_snapshot_uses_immutable_revision(self) -> None:
        observed: list[Any] = []

        def open_response(request: Request, timeout: int) -> io.BytesIO:
            observed.append(request)
            self.assertGreater(timeout, 0)
            if request.full_url.startswith("https://api.github.com/"):
                return io.BytesIO(b'{"number": 123}')
            return io.BytesIO(archive_bytes(BEFORE, repository="contributor/redis-renamed"))

        opener = Mock()
        opener.open.side_effect = open_response
        with patch.object(scanner, "build_opener", return_value=opener):
            github = scanner.GitHub("test-secret-token")
            self.assertEqual(github.api("pulls/123"), {"number": 123})
            commands, _ = github.snapshot(BEFORE, "contributor/redis-renamed")
        self.assertEqual(set(commands), {"GET", "PING"})
        self.assertEqual(observed[0].get_header("Authorization"), "Bearer test-secret-token")
        self.assertIsNone(observed[1].get_header("Authorization"))
        self.assertEqual(observed[1].full_url, f"https://codeload.github.com/contributor/redis-renamed/tar.gz/{BEFORE}")

    def test_inaccessible_fork_error_identifies_repository_and_revision(self) -> None:
        with patch.object(scanner.GitHub, "download", side_effect=scanner.ScanError("api", "GitHub HTTP 404")):
            with self.assertRaises(scanner.ScanError) as caught:
                scanner.GitHub().snapshot(HEAD, "contributor/redis-renamed")
        self.assertEqual(caught.exception.kind, "api")
        self.assertIn("contributor/redis-renamed", str(caught.exception))
        self.assertIn(HEAD, str(caught.exception))
        self.assertIn("404", str(caught.exception))

    def test_cross_host_redirect_is_not_followed(self) -> None:
        handler: HTTPRedirectHandler = scanner.NoRedirect()
        request = Request("https://api.github.com/repos/redis/redis/pulls/123", headers={"Authorization": "secret"})
        redirected = handler.redirect_request(
            request, io.BytesIO(), 302, "Found", HTTPMessage(), "https://untrusted.example/"
        )
        self.assertIsNone(redirected)

    def test_http_network_and_oversized_response_fail_without_leaking_remote_data(self) -> None:
        errors = [
            HTTPError("https://api.github.com/", 403, "secret response text", Message(), None),
            HTTPError("https://api.github.com/", 302, "secret response text", Message(), None),
            URLError("secret transport details"),
            TimeoutError("secret timeout details"),
        ]
        for error in errors:
            with self.subTest(error=type(error).__name__):
                opener = Mock()
                opener.open.side_effect = error
                with patch.object(scanner, "build_opener", return_value=opener):
                    with self.assertRaises(scanner.ScanError) as caught:
                        scanner.GitHub("secret").api("pulls/123")
                self.assertEqual(caught.exception.kind, "api")
                self.assertNotIn("secret", str(caught.exception))
        opener = Mock()
        opener.open.return_value = io.BytesIO(b"12345")
        with patch.object(scanner, "build_opener", return_value=opener):
            with self.assertRaises(scanner.ScanError) as caught:
                scanner.GitHub().download("https://api.github.com/", 4)
        self.assertEqual(caught.exception.kind, "api")

    def test_malformed_json_and_wrong_api_shapes_are_errors(self) -> None:
        for data in (b"invalid", b"[]", b"null", b'{"number": 1, "number": 2}', b'{"value": NaN}'):
            with self.subTest(data=data), patch.object(scanner.GitHub, "download", return_value=data):
                with self.assertRaises(scanner.ScanError) as caught:
                    scanner.GitHub().api("pulls/123")
                self.assertEqual(caught.exception.kind, "api")


class RunnerTests(unittest.TestCase):
    def run_main(self, github: FakeGitHub | Mock, pr_number: str = str(PR_NUMBER)) -> tuple[int, dict[str, Any], str]:
        with tempfile.TemporaryDirectory() as directory:
            report_path = Path(directory) / "report.json"
            summary_path = Path(directory) / "summary.md"
            stdout = io.StringIO()
            with patch.object(scanner, "GitHub", return_value=github), contextlib.redirect_stdout(stdout):
                status = scanner.main([pr_number, "--report", str(report_path), "--summary", str(summary_path)])
            self.last_stdout = json.loads(stdout.getvalue())
            return status, json.loads(report_path.read_text()), summary_path.read_text()

    def test_findings_and_no_findings_both_exit_successfully(self) -> None:
        for after, expect_findings in ((COMMANDS, False), ({"GET": COMMANDS["GET"]}, True)):
            with self.subTest(findings=expect_findings):
                status, report, summary = self.run_main(FakeGitHub(after))
                self.assertEqual(status, 0)
                self.assertEqual(report["status"], "completed")
                self.assertEqual(bool(report["findings"]), expect_findings)
                self.assertIs(report["has_findings"], expect_findings)
                self.assertEqual(report["finding_counts"]["total"], len(report["findings"]))
                self.assertEqual(report["finding_counts"]["potential_breaking"], int(expect_findings))
                self.assertEqual(report["finding_counts"]["review_required"], 0)
                self.assertEqual(report["errors"], [])
                self.assertFalse(report["truncated"])
                self.assertEqual(report["truncated_fields"], [])
                self.assertEqual(report["checker_version"], scanner.CHECKER_VERSION)
                self.assertEqual(report["repository"], "redis/redis")
                self.assertTrue(report["generated_at"])
                self.assertIn("human review", summary)
                self.assertIn("JSON artifact", summary)

    def test_review_only_findings_have_explicit_counts_and_summary(self) -> None:
        after = copy.deepcopy(COMMANDS)
        after["GET"]["acl_categories"] = ["READ"]
        status, report, summary = self.run_main(FakeGitHub(after))
        self.assertEqual(status, 0)
        self.assertIs(report["has_findings"], True)
        self.assertEqual(report["finding_counts"], {"total": 1, "potential_breaking": 0, "review_required": 1})
        self.assertIn("Review needed", summary)
        self.assertIn("0 potential breaking; 1 require review", summary)

    def test_finding_limits_fail_explicitly_without_a_partial_success(self) -> None:
        after = copy.deepcopy(COMMANDS)
        after["GET"]["reply_schema"] = {"properties": {"x": {"const": "x" * 10000}}}
        for setting, value in (("MAX_FINDINGS", 0), ("MAX_FINDING_BYTES", 1000)):
            with self.subTest(setting=setting), patch.object(scanner, setting, value):
                status, report, summary = self.run_main(FakeGitHub(after))
            self.assertEqual(status, 1)
            self.assertEqual(report["status"], "incomplete")
            self.assertTrue(report["truncated"])
            self.assertEqual(report["truncated_fields"], ["findings"])
            self.assertEqual(report["findings"], [])
            self.assertIsNone(report["has_findings"])
            self.assertIsNone(report["finding_counts"])
            self.assertEqual(report["errors"][0]["kind"], "scanner")
            self.assertEqual(report["revisions"]["after_sha"], HEAD)
            self.assertTrue(report["snapshots"]["after"]["files"])
            self.assertIn("Evidence omitted", summary)
            self.assertNotIn("No command-contract changes detected", summary)

    def test_report_limit_writes_bounded_incomplete_report_and_summary(self) -> None:
        after = copy.deepcopy(COMMANDS)
        after["GET"]["reply_schema"] = {"properties": {"x": {"const": "x" * 10000}}}
        with patch.object(scanner, "MAX_REPORT_BYTES", 4096):
            status, report, summary = self.run_main(FakeGitHub(after))
            self.assertLessEqual(len(scanner.bounded_report_json(report).encode()), 4096)
        self.assertEqual(status, 1)
        self.assertEqual(report["status"], "incomplete")
        self.assertTrue(report["truncated"])
        self.assertEqual(report["truncated_fields"], ["findings"])
        self.assertEqual(report["findings"], [])
        self.assertIsNone(report["has_findings"])
        self.assertIsNone(report["finding_counts"])
        self.assertEqual(report["errors"][0]["kind"], "reporting")
        self.assertEqual(report["revisions"]["after_sha"], HEAD)
        for snapshot in report["snapshots"].values():
            self.assertEqual(len(snapshot["files"]), 1)
            self.assertEqual(set(snapshot["files"][0]), {"path", "sha256", "bytes"})
            self.assertEqual(snapshot["commands"], ["GET", "PING"])
        self.assertIn("Incomplete analysis", summary)
        self.assertIn("Evidence omitted", summary)
        self.assertNotIn("Review needed", summary)
        self.assertIsNone(self.last_stdout["findings"])
        self.assertTrue(self.last_stdout["truncated"])

    def test_report_limit_also_bounds_inventories_and_error_paths(self) -> None:
        github = FakeGitHub()
        with patch.object(github, "snapshot", return_value=(COMMANDS, [{"path": "x" * 10000}])):
            with patch.object(scanner, "MAX_REPORT_BYTES", 4096):
                status, report, _ = self.run_main(github)
                self.assertLessEqual(len(scanner.bounded_report_json(report)), 4096)
        self.assertEqual(status, 1)
        self.assertEqual(
            report["truncated_fields"],
            [
                "snapshots.before.files",
                "snapshots.before.commands",
                "snapshots.after.files",
                "snapshots.after.commands",
            ],
        )
        self.assertNotIn("files", report["snapshots"]["before"])
        self.assertEqual(report["snapshots"]["before"]["file_count"], 1)
        github_failure = Mock()
        github_failure.api.side_effect = scanner.ScanError("scanner", "x" * 10000)
        with patch.object(scanner, "MAX_REPORT_BYTES", 4096):
            status, report, _ = self.run_main(github_failure)
            self.assertLessEqual(len(scanner.bounded_report_json(report)), 4096)
        self.assertEqual(status, 1)
        self.assertEqual(report["truncated_fields"], ["error_messages"])
        self.assertIn("message truncated", report["errors"][0]["message"])

    def test_report_byte_boundary_counts_ascii_escaping_and_newline(self) -> None:
        report = {"value": "雪" * 10}
        expected = json.dumps(report, ensure_ascii=True, separators=(",", ":")) + "\n"
        with patch.object(scanner, "MAX_REPORT_BYTES", len(expected)):
            self.assertEqual(scanner.bounded_report_json(report), expected)
        with patch.object(scanner, "MAX_REPORT_BYTES", len(expected) - 1):
            with self.assertRaises(scanner.ReportLimitError):
                scanner.bounded_report_json(report)

    def test_compact_nested_evidence_fits_without_losing_findings(self) -> None:
        nested: Any = list(range(100))
        for _ in range(20):
            nested = {"items": nested}
        after = copy.deepcopy(COMMANDS)
        after["GET"]["reply_schema"] = {"properties": {"x": {"const": nested}}}
        with patch.object(scanner, "MAX_REPORT_BYTES", 4096):
            status, report, _ = self.run_main(FakeGitHub(after))
            self.assertLessEqual(len(scanner.bounded_report_json(report)), 4096)
        self.assertGreater(len(json.dumps(report, indent=2)), 4096)
        self.assertEqual(status, 0)
        self.assertFalse(report["truncated"])
        self.assertEqual(len(report["findings"]), 1)

    def test_finding_byte_budget_includes_attribution_and_array_punctuation(self) -> None:
        after = copy.deepcopy(COMMANDS)
        after["GET"]["arity"] = 3
        after["PING"]["arity"] = 2
        with patch.object(scanner, "datetime") as resolved_time:
            resolved_time.now.return_value.isoformat.return_value = "2026-09-07T00:00:00+00:00"
            _, complete, _ = self.run_main(FakeGitHub(after))
            self.assertEqual(len(complete["findings"]), 2)
            size = len(json.dumps(complete["findings"], ensure_ascii=True, separators=(",", ":")))
            without_attribution = [
                {key: value for key, value in finding.items() if key not in {"pr_url", "revisions"}}
                for finding in complete["findings"]
            ]
            self.assertLess(len(json.dumps(without_attribution, separators=(",", ":"))), size - 1)
            with patch.object(scanner, "MAX_FINDING_BYTES", size):
                status, report, _ = self.run_main(FakeGitHub(after))
                self.assertEqual(status, 0)
                self.assertEqual(report["findings"], complete["findings"])
            with patch.object(scanner, "MAX_FINDING_BYTES", size - 1):
                status, report, _ = self.run_main(FakeGitHub(after))
                self.assertEqual(status, 1)
                self.assertTrue(report["truncated"])
                self.assertEqual(report["errors"][0]["kind"], "scanner")
                self.assertEqual(report["findings"], [])
                self.assertIsNone(report["finding_counts"])

    def test_late_scanner_failure_cannot_leave_a_completed_result(self) -> None:
        for error in (RuntimeError("sensitive internals"), scanner.ScanError("scanner", "counting failed")):
            with self.subTest(error=type(error).__name__):
                with patch.object(scanner, "finding_counts", side_effect=error):
                    status, report, summary = self.run_main(FakeGitHub({"GET": COMMANDS["GET"]}))
                self.assertEqual(status, 1)
                self.assertEqual(report["status"], "incomplete")
                self.assertIsNone(report["has_findings"])
                self.assertIsNone(report["finding_counts"])
                self.assertTrue(report["findings"])
                self.assertEqual(report["errors"][0]["kind"], "scanner")
                self.assertIn("Incomplete analysis", summary)
                self.assertNotIn("sensitive internals", json.dumps(report) + summary)

    def test_api_input_and_scanner_errors_fail_and_have_distinct_error_reports(self) -> None:
        for kind in ("api", "input", "scanner", "unanalyzable"):
            with self.subTest(kind=kind):
                github = Mock()
                github.api.side_effect = scanner.ScanError(kind, "synthetic failure")
                status, report, summary = self.run_main(github)
                self.assertEqual(status, 1)
                self.assertEqual(report["status"], "incomplete")
                self.assertEqual(report["errors"][0]["kind"], kind)
                self.assertIsNone(report["has_findings"])
                self.assertIsNone(report["finding_counts"])
                self.assertIn("Incomplete analysis", summary)
                self.assertNotIn("No command-contract changes detected", summary)

    def test_invalid_input_still_writes_an_error_report(self) -> None:
        status, report, _ = self.run_main(FakeGitHub(), "not-a-pr")
        self.assertEqual(status, 1)
        self.assertEqual(report["errors"][0]["kind"], "input")

    def test_after_snapshot_failure_preserves_before_inventory_and_revisions(self) -> None:
        github = FakeGitHub()
        original = github.snapshot

        def snapshot(
            revision: str, repository: str = scanner.REPOSITORY
        ) -> tuple[dict[str, Any], list[dict[str, Any]]]:
            if revision == HEAD:
                raise scanner.ScanError("api", "Synthetic archive failure")
            return original(revision, repository)

        with patch.object(github, "snapshot", side_effect=snapshot):
            status, report, summary = self.run_main(github)
        self.assertEqual(status, 1)
        self.assertEqual(report["revisions"]["before_sha"], BEFORE)
        self.assertEqual(report["revisions"]["after_sha"], HEAD)
        self.assertTrue(report["snapshots"]["before"]["files"])
        self.assertNotIn("after", report["snapshots"])
        self.assertIn("Incomplete analysis", summary)

    def test_unexpected_scanner_failure_does_not_expose_exception_contents(self) -> None:
        github = Mock()
        github.api.side_effect = RuntimeError("secret credential")
        status, report, summary = self.run_main(github)
        self.assertEqual(status, 1)
        self.assertEqual(report["errors"][0]["kind"], "scanner")
        self.assertNotIn("secret credential", json.dumps(report) + summary)

    def test_reporting_failure_salvages_writable_output_and_fails(self) -> None:
        for failed_output in ("report", "summary"):
            with self.subTest(failed_output=failed_output), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                paths = {"report": root / "report.json", "summary": root / "summary.md"}
                paths[failed_output] = root / "nonexistent" / "output"
                with patch.object(scanner, "GitHub", return_value=FakeGitHub()):
                    with contextlib.redirect_stdout(io.StringIO()):
                        status = scanner.main(
                            [str(PR_NUMBER), "--report", str(paths["report"]), "--summary", str(paths["summary"])]
                        )
                self.assertEqual(status, 1)
                if failed_output == "summary":
                    report = json.loads(paths["report"].read_text())
                    self.assertEqual(report["status"], "incomplete")
                    self.assertEqual(report["errors"][0]["kind"], "reporting")
                    self.assertIsNone(report["has_findings"])
                    self.assertIsNone(report["finding_counts"])
                else:
                    self.assertIn("Incomplete analysis", paths["summary"].read_text())

    def test_output_builders_recover_independently_after_repeated_failures(self) -> None:
        for broken_report, broken_summary in ((True, False), (False, True), (True, True)):
            with self.subTest(report=broken_report, summary=broken_summary), contextlib.ExitStack() as patches:
                if broken_report:
                    patches.enter_context(
                        patch.object(
                            scanner, "bounded_report_json", side_effect=scanner.ReportLimitError("sensitive details")
                        )
                    )
                if broken_summary:
                    patches.enter_context(patch.object(scanner, "render_summary", side_effect=RuntimeError("secret")))
                status, report, summary = self.run_main(FakeGitHub())
            self.assertEqual(status, 1)
            self.assertEqual(report["status"], "incomplete")
            self.assertIsNone(report["has_findings"])
            self.assertIsNone(report["finding_counts"])
            self.assertEqual(report["errors"][0]["kind"], "reporting")
            self.assertIn("Incomplete analysis", summary)
            self.assertNotIn("sensitive", json.dumps(report) + summary)
            self.assertNotIn("secret", json.dumps(report) + summary)
            self.assertIsNone(self.last_stdout["findings"])
            if broken_report:
                self.assertTrue(report["truncated"])
                self.assertEqual(report["truncated_fields"], ["original_report"])
            else:
                self.assertEqual(report["revisions"]["after_sha"], HEAD)
                self.assertTrue(report["snapshots"]["before"]["files"])

    def test_nonfinite_internal_value_uses_valid_json_emergency_report(self) -> None:
        original = scanner.scan

        def corrupt(pr_number: str, github: scanner.GitHub, report: dict[str, Any]) -> None:
            original(pr_number, github, report)
            report["unexpected_internal_value"] = float("nan")

        with patch.object(scanner, "scan", side_effect=corrupt):
            status, report, summary = self.run_main(FakeGitHub())
        self.assertEqual(status, 1)
        self.assertTrue(report["truncated"])
        self.assertIsNone(report["finding_counts"])
        serialized = json.dumps(report).encode()
        self.assertEqual(scanner.parse_json(serialized), report)
        self.assertNotIn(b"NaN", serialized)
        self.assertIn("Incomplete analysis", summary)

    def test_non_oserror_write_failure_does_not_prevent_other_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            report_path, summary_path = Path(directory) / "report.json", Path(directory) / "summary.md"
            original = Path.write_text

            def write(path: Path, data: str, *args: Any, **kwargs: Any) -> int:
                if path == report_path:
                    raise ValueError("sensitive path failure")
                return original(path, data, *args, **kwargs)

            with patch.object(scanner, "GitHub", return_value=FakeGitHub()), patch.object(Path, "write_text", write):
                with contextlib.redirect_stdout(io.StringIO()) as stdout:
                    status = scanner.main(
                        [str(PR_NUMBER), "--report", str(report_path), "--summary", str(summary_path)]
                    )
            self.assertEqual(status, 1)
            self.assertIn("Incomplete analysis", summary_path.read_text())
            self.assertNotIn("sensitive", summary_path.read_text())
            self.assertIsNone(json.loads(stdout.getvalue())["findings"])

    def test_summary_escapes_untrusted_fields_and_bounds_output(self) -> None:
        payload = '<img src="https://untrusted.example/pixel">![image](https://untrusted.example/)\n# injected'
        report = fresh_report()
        report["status"] = "completed"
        report["findings"] = [
            {
                "classification": "requires review",
                "command": payload,
                "field": payload,
                "old": payload,
                "new": payload,
                "explanation": payload,
            }
        ] * 60
        summary = scanner.render_summary(report)
        self.assertNotIn("<img", summary)
        self.assertNotIn("![image]", summary)
        self.assertNotIn("https://untrusted.example", summary)
        self.assertNotIn("\n# injected", summary)
        self.assertIn("Summary shows 50 of 60 findings", summary)
        self.assertEqual(summary.count("### requires review"), 50)

    def test_summary_byte_budget_accounts_for_escaped_metadata(self) -> None:
        report = fresh_report()
        report["status"] = "completed"
        report["findings"] = [
            {
                "classification": "review_required",
                **{field: "\\" * 2000 for field in ("command", "field", "old", "new", "explanation")},
            }
        ] * 50
        summary = scanner.render_summary(report)
        self.assertLessEqual(len(summary.encode("utf-8")), scanner.MAX_SUMMARY_BYTES)
        self.assertLess(summary.count("### review_required"), 50)
        self.assertIn("Download the JSON report for every finding", summary)
        self.assertEqual(len(report["findings"]), 50)


if __name__ == "__main__":
    unittest.main()
