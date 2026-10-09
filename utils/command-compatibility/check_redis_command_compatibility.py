"""Conservative, data-only comparison of Redis command JSON contracts.

Findings describe possible compatibility risks in command metadata, not proof of
runtime behavior. This module never imports or executes code from a snapshot.
"""

from __future__ import annotations

import json
import math
from collections import Counter
from collections.abc import Iterator
from difflib import SequenceMatcher
from typing import Any

DOCUMENTATION_FIELDS = {
    "summary",
    "complexity",
    "since",
    "deprecated_since",
    "replaced_by",
    "history",
    "group",
    "doc_flags",
    "description",
    "display_text",
}
ARGUMENT_MODIFIERS = {"optional", "multiple", "multiple_token"}
ARGUMENT_SYNTAX = ARGUMENT_MODIFIERS | {"type", "token", "arguments"}
STRUCTURAL_TYPES = {"block", "oneof"}
DEFAULT_MAX_FINDINGS = 1000
DEFAULT_MAX_FINDING_BYTES = 8 * 1024 * 1024
DEFAULT_MAX_WORK = 100_000


class WorkLimitError(ValueError):
    """Comparison work was exhausted before a complete result was available."""


class WorkBudget:
    """Shared by emitted findings and speculative probes, including empty ones."""

    def __init__(self, limit: int) -> None:
        self.remaining = limit

    def charge(self, units: int = 1) -> None:
        self.remaining -= units
        if self.remaining < 0:
            raise WorkLimitError("Comparison work limit exceeded; analysis is incomplete.")

    def encoded(self, value: Any) -> str:
        self.charge()
        result = _encoded(value)
        self.charge(len(result) // 1024)
        return result


class FindingLimitError(ValueError):
    """The comparison exceeded its finding budget and is incomplete."""

    def __init__(self, max_findings: int, *, max_finding_bytes: int | None = None) -> None:
        self.max_findings = max_findings
        self.max_finding_bytes = max_finding_bytes
        limit = (
            f"encoded finding byte limit of {max_finding_bytes}"
            if max_finding_bytes is not None
            else f"finding limit of {max_findings}"
        )
        super().__init__(f"Comparison exceeded the {limit}; analysis is incomplete.")


def _has_findings(findings: Iterator[dict[str, Any]]) -> bool:
    """Probe lazy containment comparisons without relying on iterator truthiness."""
    return next(findings, None) is not None


def _validate_json(value: Any, path: str, depth: int = 0) -> None:
    if depth > 60:
        raise ValueError(f"{path}: JSON nesting exceeds the scanner limit of 60")
    if value is None or isinstance(value, (str, bool, int)):
        return
    if isinstance(value, float) and math.isfinite(value):
        return
    if isinstance(value, list):
        for index, item in enumerate(value):
            _validate_json(item, f"{path}[{index}]", depth + 1)
        return
    if isinstance(value, dict) and all(isinstance(key, str) for key in value):
        for key, item in value.items():
            _validate_json(item, f"{path}.{key}", depth + 1)
        return
    raise ValueError(f"{path}: expected a finite JSON value with string object keys")


def _canonical(value: Any, ignored: frozenset[str] = frozenset()) -> Any:
    if isinstance(value, dict):
        return {key: _canonical(value[key], ignored) for key in sorted(value) if key not in ignored}
    if isinstance(value, list):
        return [_canonical(item, ignored) for item in value]
    return value


def _encoded(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def _schema(value: Any) -> Any:
    """Ignore schema prose without dropping property names or literal JSON data."""
    if not isinstance(value, dict):
        return _canonical(value)
    result: dict[str, Any] = {}
    schema_maps = {"properties", "patternProperties", "$defs", "definitions", "dependentSchemas"}
    schema_arrays = {"allOf", "anyOf", "oneOf", "prefixItems"}
    schema_values = {
        "items",
        "additionalItems",
        "additionalProperties",
        "unevaluatedItems",
        "unevaluatedProperties",
        "contains",
        "propertyNames",
        "not",
        "if",
        "then",
        "else",
    }
    for key, item in sorted(value.items()):
        if key in {"description", "title", "examples", "$comment"}:
            continue
        if key in schema_maps and isinstance(item, dict):
            result[key] = {name: _schema(child) for name, child in sorted(item.items())}
        elif key in schema_arrays and isinstance(item, list):
            result[key] = [_schema(child) for child in item]
        elif key in schema_values:
            result[key] = [_schema(child) for child in item] if isinstance(item, list) else _schema(item)
        else:
            result[key] = _canonical(item)
    return result


def _identifier(value: Any, path: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{path}: expected a nonempty command name")
    normalized = " ".join(value.replace("|", " ").split()).upper()
    if not normalized:
        raise ValueError(f"{path}: expected a nonempty command name")
    return normalized


def _arguments(value: Any, path: str, choices: bool = False) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        raise ValueError(f"{path}: expected an argument array")
    normalized = []
    for index, raw in enumerate(value):
        location = f"{path}[{index}]"
        if not isinstance(raw, dict):
            raise ValueError(f"{location}: expected an argument object")
        if not isinstance(raw.get("type"), str) or not raw["type"]:
            raise ValueError(f"{location}.type: expected a nonempty string")
        if "name" in raw and (not isinstance(raw["name"], str) or not raw["name"]):
            raise ValueError(f"{location}.name: expected a nonempty string")
        arg = {key: _canonical(item) for key, item in raw.items() if key not in DOCUMENTATION_FIELDS | {"name"}}
        for modifier in ARGUMENT_MODIFIERS:
            modifier_value = raw.get(modifier, False)
            # Historical command JSON (e.g. BITFIELD in Redis 7.0) used the
            # string "true" for these flags. It was truthy in the generator.
            # Accept only this known encoding; "false" is also truthy in
            # Python, so interpreting it as False would invent semantics.
            if modifier_value == "true":
                modifier_value = True
            if not isinstance(modifier_value, bool):
                raise ValueError(f"{location}.{modifier}: expected a boolean")
            arg[modifier] = modifier_value
        if "token" in raw:
            if not isinstance(raw["token"], str) or not raw["token"].strip():
                raise ValueError(f"{location}.token: expected a nonempty string")
            arg["token"] = raw["token"].upper()
        if raw["type"] == "pure-token" and "token" not in raw:
            # Some Redis definitions (e.g. SENTINEL SIMULATE-FAILURE) use name
            # for a literal pure-token. Here name is syntax, not a display label.
            if not isinstance(raw.get("name"), str) or not raw["name"].strip():
                raise ValueError(f"{location}: a pure-token argument requires a token or name")
            arg["token"] = raw["name"].upper()
        if "key_spec_index" in raw and (type(raw["key_spec_index"]) is not int or raw["key_spec_index"] < 0):
            raise ValueError(f"{location}.key_spec_index: expected a nonnegative integer")
        if raw["type"] in STRUCTURAL_TYPES:
            if not raw.get("arguments"):
                raise ValueError(f"{location}: {raw['type']} requires nonempty arguments")
            arg["arguments"] = _arguments(raw["arguments"], f"{location}.arguments", raw["type"] == "oneof")
            if raw["type"] == "oneof":
                # Choices are unordered; the sequence inside each choice remains ordered.
                arg["arguments"] = sorted(arg["arguments"], key=_encoded)
        elif "arguments" in raw:
            raise ValueError(f"{location}: only block/oneof arguments may contain arguments")
        plain_group = (
            arg["type"] in STRUCTURAL_TYPES
            and not any(arg[modifier] for modifier in ARGUMENT_MODIFIERS)
            and set(arg) <= ARGUMENT_SYNTAX
            and "token" not in arg
        )
        if plain_group and ((arg["type"] == "block" and not choices) or len(arg["arguments"]) == 1):
            normalized.extend(arg["arguments"])
        else:
            normalized.append(arg)
    return normalized


def normalize_snapshot(
    files: dict[str, Any], *, unanalyzable: list[dict[str, str]] | None = None
) -> dict[str, dict[str, Any]]:
    """Validate parsed JSON files and normalize command identities and syntax.

    File names are provenance only: moving a command between files has no
    contract effect. Flat ``container`` definitions and nested ``subcommands``
    objects share the same case-insensitive command identity.
    With an ``unanalyzable`` collector, unsupported contracts are excluded, but
    malformed JSON and ambiguous identities still raise. Without it, stay strict.
    """
    if not isinstance(files, dict):
        raise ValueError("snapshot: expected a map from source file paths to JSON objects")
    if any(not isinstance(path, str) for path in files):
        raise ValueError("snapshot: source file paths must be strings")
    definitions: dict[str, tuple[dict[str, Any], str, str]] = {}

    def identify(name: str, raw: Any, location: str, path: str, parent: str = "") -> None:
        if not isinstance(raw, dict):
            raise ValueError(f"{location}: expected a command object")
        container = _identifier(raw["container"], f"{location}.container") if "container" in raw else parent
        if parent and container != parent:
            raise ValueError(f"{location}: nested command container does not match its parent")
        command = _identifier(name, location)
        if container and not command.startswith(container + " "):
            command = f"{container} {command}"
        if command in definitions:
            raise ValueError(f"{location}: duplicate normalized command identity {command}")
        definitions[command] = (raw, location, path)
        if "subcommands" in raw:
            if not isinstance(raw["subcommands"], dict):
                raise ValueError(f"{location}.subcommands: expected an object")
            for child, definition in raw["subcommands"].items():
                identify(child, definition, f"{location}.subcommands.{child}", path, command)

    def normalize(raw: dict[str, Any], location: str) -> dict[str, Any]:
        if type(raw.get("arity")) is not int or raw["arity"] == 0:
            raise ValueError(f"{location}.arity: expected a nonzero integer")
        result = {
            key: _canonical(value)
            for key, value in raw.items()
            if key not in DOCUMENTATION_FIELDS | {"container", "subcommands", "arguments"}
        }
        for field in ("command_flags", "acl_categories"):
            if field in raw:
                if not isinstance(raw[field], list) or any(not isinstance(item, str) for item in raw[field]):
                    raise ValueError(f"{location}.{field}: expected an array of strings")
                result[field] = sorted(set(raw[field]))
        if "key_specs" in raw:
            if not isinstance(raw["key_specs"], list) or any(not isinstance(item, dict) for item in raw["key_specs"]):
                raise ValueError(f"{location}.key_specs: expected an array of objects")
            result["key_specs"] = []
            for spec in raw["key_specs"]:
                normalized_spec = {key: _canonical(value) for key, value in spec.items() if key != "notes"}
                if "flags" in spec:
                    if not isinstance(spec["flags"], list) or any(not isinstance(flag, str) for flag in spec["flags"]):
                        raise ValueError(f"{location}.key_specs.flags: expected an array of strings")
                    normalized_spec["flags"] = sorted(set(spec["flags"]))
                result["key_specs"].append(normalized_spec)
        if "reply_schema" in raw:
            if not isinstance(raw["reply_schema"], dict):
                raise ValueError(f"{location}.reply_schema: expected an object")
            result["reply_schema"] = _schema(raw["reply_schema"])
        result["arguments"] = _arguments(raw.get("arguments", []), f"{location}.arguments")
        return _canonical(result)

    for path, data in sorted(files.items()):
        _validate_json(data, path)
        if not isinstance(data, dict) or not data:
            raise ValueError(f"{path}: expected a nonempty command object map")
        for name, definition in sorted(data.items()):
            identify(name, definition, f"{path}:{name}", path)
    # Resolve every identity before allowing per-command exclusions. Otherwise
    # a malformed/duplicate identity could masquerade as a removed command.
    commands: dict[str, dict[str, Any]] = {}
    for command, (raw, location, path) in sorted(definitions.items()):
        try:
            commands[command] = normalize(raw, location)
        except ValueError as error:
            if unanalyzable is None:
                raise
            unanalyzable.append({"command": command, "path": path, "reason": str(error)})
    return commands


def _finding(
    command: str, field: str, old: Any, new: Any, explanation: str, classification: str = "potential_breaking"
) -> dict[str, Any]:
    return {
        "command": command,
        "field": field,
        "old": old,
        "new": new,
        "explanation": explanation,
        "classification": classification,
    }


def _tokens(argument: dict[str, Any]) -> list[str]:
    tokens = [argument["token"]] if "token" in argument else []
    for child in argument.get("arguments", []):
        tokens.extend(_tokens(child))
    return tokens


def _unique_token_matches(before: list[dict[str, Any]], after: list[dict[str, Any]]) -> dict[int, int]:
    """Find unambiguous identities; callers must still compare their order."""
    old_owners: dict[str, list[int]] = {}
    new_owners: dict[str, list[int]] = {}
    for arguments, owners in [(before, old_owners), (after, new_owners)]:
        for index, argument in enumerate(arguments):
            for token in _tokens(argument):
                owners.setdefault(token, []).append(index)
    candidates: dict[int, set[int]] = {}
    reverse: dict[int, set[int]] = {}
    for token in old_owners.keys() & new_owners.keys():
        if len(old_owners[token]) == len(new_owners[token]) == 1:
            old_index, new_index = old_owners[token][0], new_owners[token][0]
            candidates.setdefault(old_index, set()).add(new_index)
            reverse.setdefault(new_index, set()).add(old_index)
    return {
        old_index: next(iter(new_indexes))
        for old_index, new_indexes in candidates.items()
        if len(new_indexes) == 1 and len(reverse[next(iter(new_indexes))]) == 1
    }


def _compare_sequence_gap(
    command: str,
    old_items: list[tuple[int, dict[str, Any]]],
    new_items: list[tuple[int, dict[str, Any]]],
    path: str,
    repeated_tokens: set[str],
    budget: WorkBudget,
) -> Iterator[dict[str, Any]]:
    old_arguments = [arg for _, arg in old_items]
    new_arguments = [arg for _, arg in new_items]
    ambiguous_tokens = any(repeated_tokens.intersection(_tokens(arg)) for arg in old_arguments + new_arguments)
    if old_items and new_items:
        if not ambiguous_tokens and len(old_items) == len(new_items) == 1:
            old, new = old_arguments[0], new_arguments[0]
            # Only a single tokenless position supports a positional comparison.
            # Unrelated literal options are removals/additions, not renames.
            if not _tokens(old) and not _tokens(new):
                yield from _compare_argument(command, old, new, f"{path}[{old_items[0][0]}]", budget)
                return
        independent_options = all(_tokens(arg) for arg in old_arguments + new_arguments) and not (
            {token for arg in old_arguments for token in _tokens(arg)}
            & {token for arg in new_arguments for token in _tokens(arg)}
        )
        if ambiguous_tokens or not independent_options:
            # Pointwise widening proves sequence containment without asserting
            # node identity. Never use these offsets to report a fabricated edit.
            if len(old_arguments) == len(new_arguments) and all(
                not _has_findings(_compare_argument(command, old, new, path, budget))
                for old, new in zip(old_arguments, new_arguments)
            ):
                return
            yield _finding(
                command,
                path,
                old_arguments,
                new_arguments,
                "Argument alignment is ambiguous because positional arguments, grouping, or repeated tokens changed; "
                "review the ordered syntax instead of assuming arguments at the same index share an identity.",
                "review_required",
            )
            return
    for index, arg in old_items:
        ambiguous = bool(repeated_tokens.intersection(_tokens(arg)))
        yield (
            _finding(
                command,
                f"{path}[{index}]",
                arg,
                None,
                (
                    "An argument with a repeated token was removed or reorganized; its identity requires review."
                    if ambiguous
                    else "A previously described argument was removed."
                ),
                "review_required" if ambiguous else "potential_breaking",
            )
        )
    for index, arg in new_items:
        if not arg["optional"]:
            ambiguous = bool(repeated_tokens.intersection(_tokens(arg)))
            yield (
                _finding(
                    command,
                    f"{path}[{index}]",
                    None,
                    arg,
                    (
                        "A required argument with a repeated token was added or reorganized; its identity requires review."
                        if ambiguous
                        else "A required argument was added; previously described invocations may no longer be accepted."
                    ),
                    "review_required" if ambiguous else "potential_breaking",
                )
            )
    return


def _compare_arguments(
    command: str,
    before: list[dict[str, Any]],
    after: list[dict[str, Any]],
    path: str,
    budget: WorkBudget,
    choices: bool = False,
) -> Iterator[dict[str, Any]]:
    # Charge candidate matching and SequenceMatcher's worst-case pair work
    # before entering library code or loops that may produce no findings.
    budget.charge(1 + len(before) + len(after) + len(before) * len(after))
    if choices:
        # Choice order is immaterial, but branch identity still needs evidence.
        # Reuse nested unique tokens rather than pairing the first same-type
        # block after normalization has potentially reordered the alternatives.
        identities = _unique_token_matches(before, after)
        unmatched = dict(enumerate(after))
        remaining = []
        for index, old in enumerate(before):
            old_encoded = budget.encoded(old)
            unchanged = next((index for index, item in unmatched.items() if budget.encoded(item) == old_encoded), None)
            if unchanged is not None:
                unmatched.pop(unchanged)
            elif index in identities and identities[index] in unmatched:
                match = unmatched.pop(identities[index])
                yield from (_compare_argument(command, old, match, f"{path}[{index}]", budget))
            else:
                remaining.append((index, old))
        ambiguous = []
        after_tokens = {token for arg in after for token in _tokens(arg)}
        for index, old in remaining:
            # Containment can prove additive widening without inventing identity.
            if any(not _has_findings(_compare_argument(command, old, candidate, path, budget)) for candidate in after):
                continue
            if not unmatched or (_tokens(old) and not set(_tokens(old)).intersection(after_tokens)):
                yield (_finding(command, f"{path}[{index}]", old, None, "A previously described choice was removed."))
            else:
                ambiguous.append(old)
        if ambiguous:
            yield (
                _finding(
                    command,
                    path,
                    ambiguous,
                    list(unmatched.values()),
                    "Choice alignment is ambiguous because branches lack unique tokens or repeat them; "
                    "review the alternatives instead of assuming same-type branches share an identity.",
                    "review_required",
                )
            )
        return

    identities = _unique_token_matches(before, after)
    reverse = {new: old for old, new in identities.items()}
    old_keys = [f"token:{index}" if index in identities else budget.encoded(arg) for index, arg in enumerate(before)]
    new_keys = [
        f"token:{reverse[index]}" if index in reverse else budget.encoded(arg) for index, arg in enumerate(after)
    ]
    opcodes = SequenceMatcher(a=old_keys, b=new_keys, autojunk=False).get_opcodes()
    aligned = {index for tag, start, end, _, _ in opcodes if tag == "equal" for index in range(start, end)}
    aligned_new = {index for tag, _, _, start, end in opcodes if tag == "equal" for index in range(start, end)}
    moved = {old: new for old, new in identities.items() if old not in aligned}
    old_exact, new_exact = [budget.encoded(arg) for arg in before], [budget.encoded(arg) for arg in after]
    old_counts, new_counts = Counter(old_exact), Counter(new_exact)
    for old_index, encoded in enumerate(old_exact):
        if old_index not in aligned and old_index not in moved and old_counts[encoded] == new_counts[encoded] == 1:
            new_index = new_exact.index(encoded)
            if new_index not in aligned_new and new_index not in moved.values():
                moved[old_index] = new_index
    for old_index, new_index in sorted(moved.items()):
        identifiable = old_index in identities
        optional_move = before[old_index]["optional"] or after[new_index]["optional"]
        yield (
            _finding(
                command,
                f"{path}[{old_index}].position",
                old_index,
                new_index,
                (
                    "An optional token argument moved; whether option order changes accepted syntax requires parser review."
                    if identifiable and optional_move
                    else (
                        "An argument identified by a unique token moved relative to other arguments; ordered syntax may change."
                        if identifiable
                        else "Unchanged argument syntax moved in the sequence; positional or repeated-token identity requires review."
                    )
                ),
                "potential_breaking" if identifiable and not optional_move else "review_required",
            )
        )
        yield from (_compare_argument(command, before[old_index], after[new_index], f"{path}[{old_index}]", budget))
    counts = [Counter(token for arg in arguments for token in _tokens(arg)) for arguments in (before, after)]
    repeated_tokens = {token for count in counts for token, occurrences in count.items() if occurrences > 1}
    for tag, old_start, old_end, new_start, new_end in opcodes:
        if tag == "equal":
            for old_index, new_index in zip(range(old_start, old_end), range(new_start, new_end)):
                yield from (
                    _compare_argument(command, before[old_index], after[new_index], f"{path}[{old_index}]", budget)
                )
        else:
            yield from (
                _compare_sequence_gap(
                    command,
                    [(index, before[index]) for index in range(old_start, old_end) if index not in moved],
                    [(index, after[index]) for index in range(new_start, new_end) if index not in moved.values()],
                    path,
                    repeated_tokens,
                    budget,
                )
            )
    return


def _compare_argument(
    command: str, old: dict[str, Any], new: dict[str, Any], path: str, budget: WorkBudget
) -> Iterator[dict[str, Any]]:
    if budget.encoded(old) == budget.encoded(new):
        return
    if new["type"] == "oneof" and old["type"] != "oneof" and "token" not in new:
        # A choice can contain the complete old syntax, including a literal
        # token or a whole block. Compare the branch, not its tokenless wrapper.
        if set(new) <= ARGUMENT_SYNTAX and not new["multiple_token"]:
            for choice in new["arguments"]:
                candidate = dict(choice)
                candidate["optional"] = choice["optional"] or new["optional"]
                # Repeating a choice can repeat a branch's token, whereas
                # repeating a value may consume that token only once. Keep
                # repetition at its original node when proving containment.
                if not _has_findings(_compare_argument(command, old, candidate, path, budget)):
                    return
    for field in sorted(ARGUMENT_MODIFIERS):
        if old[field] == new[field]:
            continue
        if (field == "optional" and not new[field]) or (field == "multiple" and not new[field]):
            yield (
                _finding(
                    command, f"{path}.{field}", old[field], new[field], "The described argument acceptance narrowed."
                )
            )
        elif field == "multiple_token":
            yield (
                _finding(
                    command,
                    f"{path}.{field}",
                    old[field],
                    new[field],
                    "The token syntax for repeated arguments changed.",
                )
            )
    if old.get("token") != new.get("token"):
        yield (
            _finding(
                command,
                f"{path}.token",
                old.get("token"),
                new.get("token"),
                "The argument token changed; previously described syntax may no longer be accepted.",
            )
        )
    if old["type"] == new["type"] and old["type"] in STRUCTURAL_TYPES:
        yield from (
            _compare_arguments(
                command, old["arguments"], new["arguments"], f"{path}.arguments", budget, old["type"] == "oneof"
            )
        )
    elif old["type"] != new["type"]:
        yield (
            _finding(
                command,
                f"{path}.type",
                old,
                new,
                "Argument type or structure changed; metadata alone cannot establish compatibility.",
                "review_required",
            )
        )
    for field in sorted((set(old) | set(new)) - ARGUMENT_SYNTAX):
        if budget.encoded(old.get(field)) != budget.encoded(new.get(field)) or (field in old) != (field in new):
            yield (
                _finding(
                    command,
                    f"{path}.{field}",
                    old.get(field),
                    new.get(field),
                    "Argument metadata changed and requires compatibility review.",
                    "review_required",
                )
            )
    return


def _pointer(path: str, key: str | int) -> str:
    return path + "/" + str(key).replace("~", "~0").replace("/", "~1")


def _metadata_differences(
    old: Any, new: Any, path: str, budget: WorkBudget, old_present: bool = True, new_present: bool = True
) -> Iterator[tuple[str, Any, Any, bool, bool]]:
    """Return changed JSON Pointer values without treating ordered arrays as sets."""
    budget.charge()
    if old_present != new_present:
        yield path, old, new, old_present, new_present
        return
    if budget.encoded(old) == budget.encoded(new):
        return
    if isinstance(old, dict) and isinstance(new, dict):
        for key in sorted(old.keys() | new.keys()):
            yield from _metadata_differences(
                old.get(key), new.get(key), _pointer(path, key), budget, key in old, key in new
            )
        return
    if isinstance(old, list) and isinstance(new, list):
        if len(old) == len(new):
            for index, (old_item, new_item) in enumerate(zip(old, new)):
                yield from _metadata_differences(old_item, new_item, _pointer(path, index), budget)
            return
        prefix = 0
        while prefix < min(len(old), len(new)) and budget.encoded(old[prefix]) == budget.encoded(new[prefix]):
            prefix += 1
        if prefix == len(old):
            for index in range(prefix, len(new)):
                budget.charge()
                yield _pointer(path, index), None, new[index], False, True
            return
        if prefix == len(new):
            for index in range(prefix, len(old)):
                budget.charge()
                yield _pointer(path, index), old[index], None, True, False
            return
        # Middle insertions/removals shift existing indexes: the same child
        # pointer may exist on both sides with different values. Preserve the
        # parent array so presence flags and evidence describe actual pointers.
    yield path, old, new, old_present, new_present


def _command_metadata_findings(
    command: str, field: str, old: dict[str, Any], new: dict[str, Any], budget: WorkBudget
) -> Iterator[dict[str, Any]]:
    old_present, new_present = field in old, field in new
    if old_present == new_present and budget.encoded(old.get(field)) == budget.encoded(new.get(field)):
        return
    explanations = {
        "key_specs": "Key discovery metadata changed; review key positions, access modes, and routing implications.",
        "reply_schema": "Reply schema changed; review client decoding expectations. Metadata alone does not prove runtime replies.",
        "command_flags": "Command flags changed; review execution, access, and operational assumptions.",
        "acl_categories": "ACL categories changed; review category-based permission grants and denials.",
    }
    explanation = explanations.get(field, "Command metadata changed; its compatibility impact requires review.")
    if field in {"command_flags", "acl_categories"}:
        removed = sorted(set(old.get(field, [])) - set(new.get(field, [])))
        added = sorted(set(new.get(field, [])) - set(old.get(field, [])))
        explanation += f" Removed: {_encoded(removed)}. Added: {_encoded(added)}."
    if field in {"key_specs", "reply_schema"}:
        differences = _metadata_differences(
            old.get(field), new.get(field), _pointer("", field), budget, old_present, new_present
        )
    else:
        differences = iter([(field, old.get(field), new.get(field), old_present, new_present)])
    for path, old_value, new_value, was_present, is_present in differences:
        action = "Added" if not was_present else "Removed" if not is_present else "Changed"
        finding = _finding(
            command, path, old_value, new_value, f"{action} metadata at this path. {explanation}", "review_required"
        )
        finding.update(old_present=was_present, new_present=is_present)
        yield (finding)
    return


def _iter_command_findings(
    before: dict[str, dict[str, Any]], after: dict[str, dict[str, Any]], budget: WorkBudget
) -> Iterator[dict[str, Any]]:
    """Generate normalized contract findings without collecting recursive results."""
    for command, old in sorted(before.items()):
        budget.charge()
        if command not in after:
            yield (_finding(command, "command", old, None, "A previously described command was removed."))
            continue
        new = after[command]
        old_arity, new_arity = old["arity"], new["arity"]
        narrowed = (new_arity > 0 and new_arity != old_arity) or (
            new_arity < 0 and (old_arity > 0 and old_arity < -new_arity or old_arity < 0 and new_arity < old_arity)
        )
        if narrowed:
            yield (
                _finding(
                    command,
                    "arity",
                    old_arity,
                    new_arity,
                    "The declared arity no longer accepts every previously accepted argument count.",
                )
            )
        yield from (_compare_arguments(command, old["arguments"], new["arguments"], "arguments", budget))
        for field in sorted((set(old) | set(new)) - {"arity", "arguments"}):
            yield from (_command_metadata_findings(command, field, old, new, budget))
    return


def compare_commands(
    before: dict[str, dict[str, Any]],
    after: dict[str, dict[str, Any]],
    *,
    max_findings: int = DEFAULT_MAX_FINDINGS,
    max_finding_bytes: int = DEFAULT_MAX_FINDING_BYTES,
    max_work: int = DEFAULT_MAX_WORK,
) -> list[dict[str, Any]]:
    """Compare normalized snapshots, or raise a finding/work limit error.

    key_specs/reply_schema metadata fields use RFC 6901 JSON Pointer paths;
    argument fields retain dot/index paths. Metadata old_present/new_present
    distinguish absent fields from fields whose JSON value is null.

    Findings are generated lazily and checked before collection. The byte limit
    counts compact ASCII JSON objects, excluding array delimiters and downstream
    report metadata. Work is shared with recursive probes and sequence matching.
    No limit returns or accepts a partial comparison.
    """
    for name, limit in [
        ("max_findings", max_findings),
        ("max_finding_bytes", max_finding_bytes),
        ("max_work", max_work),
    ]:
        if type(limit) is not int or limit < 0:
            raise ValueError(f"{name} must be a nonnegative integer")
    findings: list[dict[str, Any]] = []
    finding_bytes = 0
    encoder = json.JSONEncoder(ensure_ascii=True, separators=(",", ":"))
    for finding in _iter_command_findings(before, after, WorkBudget(max_work)):
        if len(findings) >= max_findings:
            raise FindingLimitError(max_findings)
        for chunk in encoder.iterencode(finding):
            # ensure_ascii makes each character exactly one encoded byte. Do
            # not join chunks: a long repeated path must not build a large copy.
            finding_bytes += len(chunk)
            if finding_bytes > max_finding_bytes:
                raise FindingLimitError(max_findings, max_finding_bytes=max_finding_bytes)
        findings.append(finding)
    return findings
