"""Offline regression tests for the advisory contract comparator."""

from __future__ import annotations

import copy
import json
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

import check_redis_command_compatibility as compatibility
from check_redis_command_compatibility import FindingLimitError, compare_commands, normalize_snapshot

FIXTURES = Path(__file__).parent / "fixtures" / "command_compatibility"


def snapshot(arguments: list[dict[str, Any]] | None = None, arity: int = -2, **fields: Any) -> dict[str, Any]:
    return {"src/commands/example.json": {"EXAMPLE": {"arity": arity, "arguments": arguments or [], **fields}}}


def argument(kind: str = "string", **fields: Any) -> dict[str, Any]:
    return {"name": "value", "type": kind, **fields}


def compare(before: dict[str, Any], after: dict[str, Any]) -> list[dict[str, Any]]:
    return compare_commands(normalize_snapshot(before), normalize_snapshot(after))


class CommandCompatibilityTests(unittest.TestCase):
    def test_legacy_true_modifiers_match_booleans_without_mutating_inputs(self) -> None:
        for modifier in sorted(compatibility.ARGUMENT_MODIFIERS):
            with self.subTest(modifier=modifier):
                old = snapshot([{**argument(), modifier: "true"}])
                original = copy.deepcopy(old)
                new = snapshot([{**argument(), modifier: True}])
                self.assertEqual(compare(old, new), [])
                self.assertEqual(compare(new, old), [])
                self.assertEqual(old, original)

    def test_legacy_true_nested_modifiers_still_detect_narrowing(self) -> None:
        for modifier in sorted(compatibility.ARGUMENT_MODIFIERS):
            with self.subTest(modifier=modifier):
                old = argument("block", token="OPTIONS", arguments=[argument(**{modifier: "true"})])
                new = copy.deepcopy(old)
                new["arguments"][0][modifier] = False
                findings = compare(snapshot([old]), snapshot([new]))
                self.assertEqual(len(findings), 1)
                self.assertEqual(findings[0]["field"], f"arguments[0].arguments[0].{modifier}")
                self.assertEqual(findings[0]["classification"], "potential_breaking")

    def test_modifier_normalization_rejects_unknown_encodings(self) -> None:
        invalid_values: list[Any] = ["false", "TRUE", "True", " true ", "1", "", 0, 1, None, [], {}]
        for modifier in sorted(compatibility.ARGUMENT_MODIFIERS):
            for value in invalid_values:
                with self.subTest(modifier=modifier, value=value), self.assertRaisesRegex(ValueError, "boolean"):
                    normalize_snapshot(snapshot([{**argument(), modifier: value}]))

    def test_compatible_fixture_covers_relocation_regrouping_names_choices_and_options(self) -> None:
        before = json.loads((FIXTURES / "before.json").read_text())
        after = json.loads((FIXTURES / "compatible.json").read_text())
        self.assertEqual(compare(before, after), [])

    def test_removal_uses_command_identity_including_container(self) -> None:
        before = {"src/commands/kill.json": {"KILL": {"container": "CLIENT", "arity": -3}}}
        findings = compare(before, {})
        self.assertEqual([(f["command"], f["field"]) for f in findings], [("CLIENT KILL", "command")])
        self.assertEqual(findings[0]["classification"], "potential_breaking")

    def test_nested_subcommands_and_flat_containers_are_equivalent(self) -> None:
        before = {"src/commands/client.json": {"CLIENT": {"arity": -2, "subcommands": {"KILL": {"arity": -3}}}}}
        after = {"src/commands/client.json": {"CLIENT": {"arity": -2}, "KILL": {"container": "client", "arity": -3}}}
        self.assertEqual(compare(before, after), [])

    def test_arity_acceptance_sets(self) -> None:
        for old, new, breaking in [
            (3, 3, False),
            (3, 4, True),
            (3, -3, False),
            (3, -2, False),
            (3, -4, True),
            (-3, -3, False),
            (-3, -2, False),
            (-3, -4, True),
            (-3, 3, True),
            (-3, 4, True),
        ]:
            with self.subTest(old=old, new=new):
                self.assertEqual(bool(compare(snapshot(arity=old), snapshot(arity=new))), breaking)

    def test_nested_argument_becoming_required(self) -> None:
        old = argument("block", optional=True, arguments=[argument("integer", token="EX", optional=True)])
        new = copy.deepcopy(old)
        new["arguments"][0]["optional"] = False
        findings = compare(snapshot([old]), snapshot([new]))
        self.assertEqual([f["field"] for f in findings], ["arguments[0].arguments[0].optional"])
        self.assertEqual(findings[0]["classification"], "potential_breaking")

    def test_optional_addition_before_same_type_required_argument_is_compatible(self) -> None:
        self.assertEqual(compare(snapshot([argument()]), snapshot([argument(optional=True), argument()])), [])

    def test_removed_optional_argument_is_still_a_contract_removal(self) -> None:
        findings = compare(snapshot([argument(optional=True)]), snapshot())
        self.assertEqual(findings[0]["classification"], "potential_breaking")

    def test_required_addition_is_reported(self) -> None:
        findings = compare(snapshot([argument()]), snapshot([argument(), argument("integer")]))
        self.assertEqual(findings[0]["classification"], "potential_breaking")

    def test_token_change_is_reported(self) -> None:
        findings = compare(snapshot([argument(token="OLD")]), snapshot([argument(token="NEW")]))
        self.assertEqual([(f["old"] is None, f["new"] is None) for f in findings], [(False, True), (True, False)])
        self.assertEqual(findings[0]["old"]["token"], "OLD")
        self.assertEqual(findings[1]["new"]["token"], "NEW")

    def test_removed_optional_option_is_not_fabricated_as_a_changed_added_option(self) -> None:
        findings = compare(
            snapshot([argument(token="OLD", optional=True)]), snapshot([argument(token="NEW", optional=True)])
        )
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0]["old"]["token"], "OLD")
        self.assertIsNone(findings[0]["new"])

    def test_unique_token_anchor_survives_adjacent_remove_add_and_modified_fields(self) -> None:
        before = [argument(token="OLD", optional=True), argument(token="KEEP", optional=True)]
        after = [argument(token="NEW", optional=True), argument(token="KEEP", optional=False)]
        findings = compare(snapshot(before), snapshot(after))
        self.assertEqual({f["field"] for f in findings}, {"arguments[0]", "arguments[1].optional"})
        self.assertIsNone(next(f for f in findings if f["field"] == "arguments[0]")["new"])

    def test_unique_tokens_reordered_with_other_edits_still_report_order(self) -> None:
        before = [argument(token="A"), argument("integer", token="B")]
        after = [argument("integer", token="B", optional=True), argument(token="A", multiple=True)]
        findings = compare(snapshot(before), snapshot(after))
        self.assertTrue(any(f["field"].endswith(".position") for f in findings))
        self.assertEqual({f["classification"] for f in findings}, {"review_required"})

    def test_optional_token_reordering_requires_parser_review(self) -> None:
        before = [argument(token="A", optional=True), argument(token="B", optional=True)]
        findings = compare(snapshot(before), snapshot(list(reversed(before))))
        self.assertEqual([f["classification"] for f in findings], ["review_required"])
        self.assertTrue(findings[0]["field"].endswith(".position"))
        self.assertIn("parser review", findings[0]["explanation"])

    def test_optional_token_prefixed_block_reorder_preserves_other_narrowing_findings(self) -> None:
        before = [
            argument("block", token="A", optional=True, arguments=[argument(multiple=True)]),
            argument("block", token="B", optional=True, arguments=[argument(multiple=True)]),
        ]
        after = copy.deepcopy(list(reversed(before)))
        for block in after:
            block["arguments"][0]["multiple"] = False
        findings = compare(snapshot(before), snapshot(after))
        moves = [f for f in findings if f["field"].endswith(".position")]
        narrowed = [f for f in findings if f["field"].endswith(".multiple")]
        self.assertEqual([f["classification"] for f in moves], ["review_required"])
        self.assertEqual([f["classification"] for f in narrowed], ["potential_breaking", "potential_breaking"])

    def test_repeated_token_changes_require_alignment_review(self) -> None:
        before = [argument(token="ITEM", optional=True), argument(token="ITEM", multiple=True)]
        after = [argument(token="ITEM"), argument(token="ITEM")]
        findings = compare(snapshot(before), snapshot(after))
        self.assertEqual([f["classification"] for f in findings], ["review_required"])
        self.assertEqual(findings[0]["field"], "arguments")
        self.assertIn("alignment is ambiguous", findings[0]["explanation"])

    def test_ambiguous_positional_range_is_not_paired_by_offset(self) -> None:
        before = [argument(optional=True), argument("integer", multiple=True)]
        after = [argument("integer", optional=True), argument(multiple=True)]
        findings = compare(snapshot(before), snapshot(after))
        self.assertEqual([f["classification"] for f in findings], ["review_required"])
        self.assertEqual(findings[0]["field"], "arguments")

    def test_positional_reordering_requires_review_instead_of_type_based_clean_result(self) -> None:
        before = [argument(), argument("integer")]
        findings = compare(snapshot(before), snapshot(list(reversed(before))))
        self.assertEqual([f["classification"] for f in findings], ["review_required"])
        self.assertTrue(findings[0]["field"].endswith(".position"))

    def test_repeatability_narrowing_and_expansion(self) -> None:
        before, after = snapshot([argument(multiple=True)]), snapshot([argument(multiple=False)])
        self.assertEqual(compare(before, after)[0]["field"], "arguments[0].multiple")
        self.assertEqual(compare(after, before), [])

    def test_multiple_token_changes_are_reported(self) -> None:
        findings = compare(
            snapshot([argument(token="ITEM", multiple=True)]),
            snapshot([argument(token="ITEM", multiple=True, multiple_token=True)]),
        )
        self.assertEqual(findings[0]["field"], "arguments[0].multiple_token")

    def test_type_change_requires_review(self) -> None:
        findings = compare(snapshot([argument()]), snapshot([argument("integer")]))
        self.assertEqual([f["classification"] for f in findings], ["review_required"])

    def test_unknown_metadata_requires_review(self) -> None:
        for before, after, field in [
            (snapshot(future_contract=1), snapshot(future_contract=2), "future_contract"),
            (snapshot([argument(minimum=1)]), snapshot([argument(minimum=2)]), "arguments[0].minimum"),
        ]:
            with self.subTest(field=field):
                self.assertEqual(compare(before, after)[0]["field"], field)
                self.assertEqual(compare(before, after)[0]["classification"], "review_required")

    def test_command_metadata_is_review_required(self) -> None:
        cases: list[tuple[str, Any, Any]] = [
            ("command_flags", ["READONLY"], ["WRITE"]),
            ("acl_categories", ["READ"], ["WRITE"]),
            ("key_specs", [{"flags": ["RO"]}], [{"flags": ["RW"]}]),
            ("reply_schema", {"type": "string"}, {"type": "integer"}),
        ]
        for field, old, new in cases:
            with self.subTest(field=field):
                findings = compare(snapshot(**{field: old}), snapshot(**{field: new}))
                expected = {"key_specs": "/key_specs/0/flags/0", "reply_schema": "/reply_schema/type"}.get(field, field)
                self.assertEqual([(f["field"], f["classification"]) for f in findings], [(expected, "review_required")])

    def test_flags_and_acl_explain_specific_risks_and_set_differences(self) -> None:
        cases = [
            ("command_flags", "READONLY", "WRITE", "operational assumptions"),
            ("acl_categories", "READ", "WRITE", "permission grants and denials"),
        ]
        for field, removed, added, risk in cases:
            old_fields: dict[str, Any] = {field: ["KEEP", removed]}
            new_fields: dict[str, Any] = {field: ["KEEP", added]}
            findings = compare(snapshot(**old_fields), snapshot(**new_fields))
            self.assertEqual(len(findings), 1)
            self.assertIn(risk, findings[0]["explanation"])
            self.assertIn(f'Removed: ["{removed}"]', findings[0]["explanation"])
            self.assertIn(f'Added: ["{added}"]', findings[0]["explanation"])
            self.assertEqual(findings[0]["old"], sorted(["KEEP", removed]))
            self.assertEqual(findings[0]["new"], sorted(["KEEP", added]))

    def test_keyspec_nested_changes_have_targeted_paths_and_values(self) -> None:
        old = [{"begin_search": {"index": {"pos": 1}}, "find_keys": {"range": {"lastkey": -1}}}]
        new = [{"begin_search": {"index": {"pos": 2}}, "find_keys": {"range": {"lastkey": -2}}}]
        findings = compare(snapshot(key_specs=old), snapshot(key_specs=new))
        self.assertEqual(
            [(f["field"], f["old"], f["new"]) for f in findings],
            [("/key_specs/0/begin_search/index/pos", 1, 2), ("/key_specs/0/find_keys/range/lastkey", -1, -2)],
        )
        self.assertTrue(all("routing implications" in f["explanation"] for f in findings))

    def test_schema_paths_escape_arbitrary_property_names_without_ambiguity(self) -> None:
        name = "a/b~c.d[0]"
        old = {"properties": {name: {"type": "string"}, "keep": {"type": "integer"}}}
        new = {"properties": {name: {"type": "null"}, "keep": {"type": "integer"}}}
        findings = compare(snapshot(reply_schema=old), snapshot(reply_schema=new))
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0]["field"], "/reply_schema/properties/a~1b~0c.d[0]/type")
        self.assertEqual((findings[0]["old"], findings[0]["new"]), ("string", "null"))
        self.assertIn("client decoding", findings[0]["explanation"])

    def test_schema_missing_and_null_are_distinct_in_evidence(self) -> None:
        for old, new, present in [({}, {"const": None}, (False, True)), ({"const": None}, {}, (True, False))]:
            findings = compare(snapshot(reply_schema=old), snapshot(reply_schema=new))
            self.assertEqual(
                [(f["field"], f["old"], f["new"]) for f in findings], [("/reply_schema/const", None, None)]
            )
            self.assertEqual((findings[0]["old_present"], findings[0]["new_present"]), present)

    def test_metadata_arrays_preserve_order_and_record_added_removed_items(self) -> None:
        old = {"prefixItems": [{"type": "string"}, {"type": "integer"}]}
        inserted = {"prefixItems": [{"type": "string"}, {"type": "boolean"}, {"type": "integer"}]}
        reordered = {"prefixItems": list(reversed(old["prefixItems"]))}
        findings = compare(snapshot(reply_schema=old), snapshot(reply_schema=inserted))
        self.assertEqual(findings[0]["field"], "/reply_schema/prefixItems")
        self.assertTrue(findings[0]["old_present"])
        self.assertTrue(findings[0]["new_present"])
        self.assertEqual(findings[0]["old"], old["prefixItems"])
        self.assertEqual(findings[0]["new"], inserted["prefixItems"])
        removed = compare(snapshot(reply_schema=inserted), snapshot(reply_schema=old))
        self.assertEqual(removed[0]["field"], "/reply_schema/prefixItems")
        self.assertTrue(removed[0]["old_present"])
        self.assertTrue(removed[0]["new_present"])
        self.assertEqual(removed[0]["old"], inserted["prefixItems"])
        self.assertEqual(removed[0]["new"], old["prefixItems"])
        changed = compare(snapshot(reply_schema=old), snapshot(reply_schema=reordered))
        self.assertEqual(
            {f["field"] for f in changed}, {"/reply_schema/prefixItems/0/type", "/reply_schema/prefixItems/1/type"}
        )

    def test_metadata_array_tail_changes_only_mark_genuinely_missing_pointers(self) -> None:
        old = {"prefixItems": [{"type": "string"}]}
        new = {"prefixItems": [{"type": "string"}, {"const": None}]}
        added = compare(snapshot(reply_schema=old), snapshot(reply_schema=new))
        self.assertEqual(added[0]["field"], "/reply_schema/prefixItems/1")
        self.assertFalse(added[0]["old_present"])
        self.assertTrue(added[0]["new_present"])
        self.assertEqual(added[0]["new"], {"const": None})
        removed = compare(snapshot(reply_schema=new), snapshot(reply_schema=old))
        self.assertEqual(removed[0]["field"], "/reply_schema/prefixItems/1")
        self.assertTrue(removed[0]["old_present"])
        self.assertFalse(removed[0]["new_present"])

    def test_json_boolean_and_number_metadata_are_not_equal(self) -> None:
        findings = compare(snapshot(reply_schema={"const": True}), snapshot(reply_schema={"const": 1}))
        self.assertEqual([(f["field"], f["old"], f["new"]) for f in findings], [("/reply_schema/const", True, 1)])
        findings = compare(snapshot([argument(future=True)]), snapshot([argument(future=1)]))
        self.assertEqual(
            [(f["field"], f["classification"]) for f in findings], [("arguments[0].future", "review_required")]
        )

    def test_schema_property_names_and_literal_data_are_not_documentation(self) -> None:
        for field in ["description", "title", "examples"]:
            for old, new in [
                ({"properties": {field: {"type": "string"}}}, {"properties": {field: {"type": "integer"}}}),
                ({"const": {field: "old"}}, {"const": {field: "new"}}),
                ({"enum": [{field: "old"}]}, {"enum": [{field: "new"}]}),
            ]:
                with self.subTest(field=field, old=old):
                    findings = compare(snapshot(reply_schema=old), snapshot(reply_schema=new))
                    self.assertEqual([f["classification"] for f in findings], ["review_required"])

    def test_schema_prose_changes_are_ignored_at_schema_positions(self) -> None:
        before = {"properties": {"value": {"type": "string", "description": "old"}}}
        after = {"properties": {"value": {"type": "string", "description": "new"}}}
        self.assertEqual(compare(snapshot(reply_schema=before), snapshot(reply_schema=after)), [])

    def test_key_spec_flag_order_is_normalized(self) -> None:
        self.assertEqual(
            compare(
                snapshot(key_specs=[{"flags": ["OW", "UPDATE"]}]), snapshot(key_specs=[{"flags": ["UPDATE", "OW"]}])
            ),
            [],
        )

    def test_legacy_pure_token_uses_its_name_as_literal_syntax(self) -> None:
        old = snapshot([argument("pure-token", name="crash-after-election")])
        explicit = snapshot([argument("pure-token", name="display", token="CRASH-AFTER-ELECTION")])
        changed = snapshot([argument("pure-token", name="crash-after-promotion")])
        self.assertEqual(compare(old, explicit), [])
        self.assertEqual(compare(old, changed)[0]["old"]["token"], "CRASH-AFTER-ELECTION")
        self.assertIsNone(compare(old, changed)[0]["new"])

    def test_choice_removal(self) -> None:
        old = argument("oneof", optional=True, arguments=[argument(token="A"), argument(token="B")])
        new = copy.deepcopy(old)
        new["arguments"].pop()
        findings = compare(snapshot([old]), snapshot([new]))
        self.assertTrue(any(f["classification"] == "potential_breaking" for f in findings))

    def test_oneof_block_preserves_sequence_in_each_choice(self) -> None:
        block = argument("block", arguments=[argument(token="A"), argument(token="B")])
        grouped = argument("oneof", arguments=[block, argument(token="C")])
        split = argument("oneof", arguments=[argument(token="A"), argument(token="B"), argument(token="C")])
        normalized = normalize_snapshot(snapshot([grouped]))["EXAMPLE"]["arguments"][0]
        self.assertTrue(any(item["type"] == "block" for item in normalized["arguments"]))
        self.assertTrue(compare(snapshot([grouped]), snapshot([split])))

    def test_oneof_nested_tokens_match_blocks_when_optional_changes_reorder_them(self) -> None:
        def branch(token: str, optional: bool) -> dict[str, Any]:
            return argument("block", arguments=[argument(optional=optional), argument("pure-token", token=token)])

        before = argument("oneof", arguments=[branch("A", True), branch("B", False)])
        after = argument("oneof", arguments=[branch("A", False), branch("B", True)])
        findings = compare(snapshot([before]), snapshot([after]))
        self.assertEqual(len(findings), 1)
        self.assertTrue(findings[0]["field"].endswith(".optional"))
        self.assertEqual((findings[0]["old"], findings[0]["new"]), (True, False))
        self.assertEqual(findings[0]["classification"], "potential_breaking")

    def test_oneof_repeated_nested_tokens_do_not_use_first_same_type_branch(self) -> None:
        def branch(kind: str, optional: bool) -> dict[str, Any]:
            return argument(
                "block", arguments=[argument(kind, optional=optional), argument("pure-token", token="ITEM")]
            )

        before = argument("oneof", token="MODE", arguments=[branch("string", True), branch("integer", True)])
        after = argument("oneof", token="MODE", arguments=[branch("string", False), branch("integer", False)])
        findings = compare(snapshot([before]), snapshot([after]))
        self.assertEqual([f["classification"] for f in findings], ["review_required"])
        self.assertIn("Choice alignment is ambiguous", findings[0]["explanation"])

    def test_argument_order_is_preserved(self) -> None:
        before = [argument(token="A"), argument(token="B")]
        findings = compare(snapshot(before), snapshot(list(reversed(before))))
        self.assertEqual([f["classification"] for f in findings], ["potential_breaking"])

    def test_optional_group_is_not_flattened_into_independent_options(self) -> None:
        grouped = argument("block", optional=True, arguments=[argument(token="A"), argument(token="B")])
        independent = [argument(token="A", optional=True), argument(token="B", optional=True)]
        self.assertNotEqual(normalize_snapshot(snapshot([grouped])), normalize_snapshot(snapshot(independent)))

    def test_existing_scalar_wrapped_in_wider_choice_is_compatible(self) -> None:
        choice = argument("oneof", arguments=[argument(), argument("integer")])
        self.assertEqual(compare(snapshot([argument()]), snapshot([choice])), [])

    def test_existing_token_wrapped_in_wider_choice_is_compatible(self) -> None:
        token = argument("pure-token", token="ON")
        choice = argument("oneof", arguments=[token, argument("pure-token", token="OFF")])
        self.assertEqual(compare(snapshot([token]), snapshot([choice])), [])

    def test_choice_wrapper_token_changes_composed_syntax_and_requires_review(self) -> None:
        token = argument("pure-token", token="ON")
        choice = argument("oneof", token="ON", arguments=[token, argument("pure-token", token="OFF")])
        findings = compare(snapshot([token]), snapshot([choice]))
        self.assertEqual([f["classification"] for f in findings], ["review_required"])

    def test_unknown_null_metadata_addition_is_not_silently_ignored(self) -> None:
        self.assertEqual(compare(snapshot(), snapshot(future=None))[0]["classification"], "review_required")
        findings = compare(snapshot([argument()]), snapshot([argument(future=None)]))
        self.assertEqual(findings[0]["classification"], "review_required")

    def test_choice_widening_preserves_optional_and_multiple_constraints(self) -> None:
        for modifier in ["optional", "multiple"]:
            token = argument("pure-token", token="ON", **{modifier: True})
            stripped = argument("pure-token", token="ON")
            choice = argument("oneof", arguments=[stripped, argument("pure-token", token="OFF")])
            self.assertTrue(compare(snapshot([token]), snapshot([choice])))
            choice[modifier] = True
            if modifier == "multiple":
                self.assertTrue(compare(snapshot([token]), snapshot([choice])))
                choice["arguments"][0]["multiple"] = True
            self.assertEqual(compare(snapshot([token]), snapshot([choice])), [])

    def test_repetition_moved_to_choice_does_not_prove_preserved_token_syntax(self) -> None:
        old = argument(token="ITEM", multiple=True)
        new = argument("oneof", multiple=True, arguments=[argument(token="ITEM"), argument("pure-token", token="ALL")])
        # ITEM v1 v2 is described before; repeating the new branch describes
        # ITEM v1 ITEM v2, which does not establish containment of old syntax.
        findings = compare(snapshot([old]), snapshot([new]))
        self.assertTrue(findings)
        self.assertTrue(any(f["field"].endswith(".type") for f in findings))

    def test_incompatible_fixture_covers_removals_arity_choices_and_nested_requiredness(self) -> None:
        before = json.loads((FIXTURES / "before.json").read_text())
        after = json.loads((FIXTURES / "incompatible.json").read_text())
        findings = compare(before, after)
        self.assertEqual({f["classification"] for f in findings}, {"potential_breaking"})
        self.assertTrue(
            {("GONE", "command"), ("CLIENT KILL", "command"), ("SET", "arity")}.issubset(
                {(f["command"], f["field"]) for f in findings}
            )
        )
        self.assertTrue(any(f["field"].endswith(".optional") for f in findings))
        self.assertTrue(any("choice was removed" in f["explanation"] for f in findings))

    def test_ambiguous_fixture_requires_review_without_claiming_runtime_breakage(self) -> None:
        before = json.loads((FIXTURES / "before.json").read_text())
        after = json.loads((FIXTURES / "ambiguous.json").read_text())
        findings = compare(before, after)
        self.assertEqual({f["classification"] for f in findings}, {"review_required"})
        self.assertTrue(
            {
                "/key_specs/0/begin_search/index/pos",
                "/reply_schema/type",
                "arguments[1].type",
                "arguments[3].type",
            }.issubset({f["field"] for f in findings})
        )

    def test_renamed_argument_is_not_a_removal(self) -> None:
        self.assertEqual(compare(snapshot([argument(name="old")]), snapshot([argument(name="new")])), [])

    def test_result_is_deterministic_and_inputs_are_unmodified(self) -> None:
        before = json.loads((FIXTURES / "before.json").read_text())
        original = copy.deepcopy(before)
        self.assertEqual(compare(before, {}), compare(dict(reversed(list(before.items()))), {}))
        self.assertEqual(before, original)
        self.assertEqual(
            set(compare(before, {})[0]), {"command", "field", "old", "new", "explanation", "classification"}
        )

    def test_finding_count_limit_stops_recursive_metadata_generation(self) -> None:
        schemas = [
            (
                {"properties": {f"p{i}": {"type": "string"} for i in range(2000)}},
                {"properties": {f"p{i}": {"type": "integer"} for i in range(2000)}},
            ),
            ({"prefixItems": [{"type": "string"}] * 2000}, {"prefixItems": [{"type": "integer"}] * 2000}),
        ]
        for old, new in schemas:
            before = normalize_snapshot(snapshot(reply_schema=old))
            after = normalize_snapshot(snapshot(reply_schema=new))
            with mock.patch.object(compatibility, "_finding", wraps=compatibility._finding) as generated:
                with self.assertRaises(FindingLimitError) as error:
                    compare_commands(before, after, max_findings=7)
                self.assertEqual(error.exception.max_findings, 7)
                self.assertEqual(generated.call_count, 8)

    def test_finding_count_limit_stops_nested_argument_generation(self) -> None:
        children = [argument(token=f"OPT{i}", optional=True) for i in range(2000)]
        before = normalize_snapshot(snapshot([argument("block", token="OPTIONS", optional=True, arguments=children)]))
        after = normalize_snapshot(
            snapshot([argument("block", token="OPTIONS", optional=True, arguments=children[:1])])
        )
        with mock.patch.object(compatibility, "_finding", wraps=compatibility._finding) as generated:
            with self.assertRaises(FindingLimitError):
                compare_commands(before, after, max_findings=5)
            self.assertEqual(generated.call_count, 6)

    def test_finding_count_exact_boundary_and_default_limit(self) -> None:
        before = normalize_snapshot({"src/commands/commands.json": {f"CMD{i}": {"arity": 1} for i in range(1001)}})
        first_three = dict(list(before.items())[:3])
        self.assertEqual(len(compare_commands(first_three, {}, max_findings=3)), 3)
        with self.assertRaises(FindingLimitError):
            compare_commands(first_three, {}, max_findings=2)
        with self.assertRaises(FindingLimitError) as error:
            compare_commands(before, {})
        self.assertEqual(error.exception.max_findings, 1000)

    def test_finding_byte_limit_exact_boundary(self) -> None:
        before = normalize_snapshot(snapshot(reply_schema={"properties": {"café": {"const": "é"}}}))
        after = normalize_snapshot(snapshot(reply_schema={"properties": {"café": {"const": "新"}}}))
        findings = compare_commands(before, after)
        encoded_bytes = sum(len(json.dumps(f, ensure_ascii=True, separators=(",", ":"))) for f in findings)
        self.assertEqual(compare_commands(before, after, max_finding_bytes=encoded_bytes), findings)
        with self.assertRaises(FindingLimitError) as error:
            compare_commands(before, after, max_finding_bytes=encoded_bytes - 1)
        self.assertEqual(error.exception.max_finding_bytes, encoded_bytes - 1)

    def test_finding_byte_limit_stops_repeated_long_paths_before_accumulation(self) -> None:
        long_name = "x" * 10000
        old = {"properties": {long_name: {"properties": {f"p{i:04}": {"type": "string"} for i in range(1000)}}}}
        new = {"properties": {long_name: {"properties": {f"p{i:04}": {"type": "integer"} for i in range(1000)}}}}
        before, after = normalize_snapshot(snapshot(reply_schema=old)), normalize_snapshot(snapshot(reply_schema=new))
        first = next(compatibility._iter_command_findings(before, after, compatibility.WorkBudget(100_000)))
        first_bytes = len(json.dumps(first, ensure_ascii=True, separators=(",", ":")))
        with mock.patch.object(compatibility, "_finding", wraps=compatibility._finding) as generated:
            with self.assertRaises(FindingLimitError):
                compare_commands(before, after, max_finding_bytes=first_bytes * 2)
            self.assertEqual(generated.call_count, 3)

    def test_containment_probes_do_not_consume_the_result_budget_or_hide_findings(self) -> None:
        old = normalize_snapshot(snapshot([argument("pure-token", token="ON")]))
        wider = normalize_snapshot(
            snapshot(
                [argument("oneof", arguments=[argument("pure-token", token="OFF"), argument("pure-token", token="ON")])]
            )
        )
        self.assertEqual(compare_commands(old, wider, max_findings=0, max_finding_bytes=0), [])
        old_optional = normalize_snapshot(snapshot([argument("pure-token", token="ON", optional=True)]))
        with self.assertRaises(FindingLimitError):
            compare_commands(old_optional, wider, max_findings=0)
        self.assertTrue(compare_commands(old_optional, wider))

    def test_finding_limits_validate_nonnegative_integer_budgets(self) -> None:
        invalid_values: list[Any] = [-1, True, 1.5]
        for field in ["max_findings", "max_finding_bytes", "max_work"]:
            for value in invalid_values:
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    compare_commands({}, {}, **{field: value})
        self.assertEqual(compare_commands({}, {}, max_findings=0, max_finding_bytes=0), [])

    def test_work_budget_bounds_choice_matching_before_any_finding(self) -> None:
        before = normalize_snapshot(snapshot([argument("oneof", arguments=[argument(tag=i) for i in range(128)])]))
        after = normalize_snapshot(snapshot([argument("oneof", arguments=[argument(tag=i + 128) for i in range(128)])]))
        with mock.patch.object(compatibility, "_finding", wraps=compatibility._finding) as findings:
            with self.assertRaises(compatibility.WorkLimitError):
                compare_commands(before, after, max_findings=0, max_work=1000)
        self.assertEqual(findings.call_count, 0)

    def test_work_budget_is_shared_with_recursive_containment_probes(self) -> None:
        choice = argument("pure-token", token="ON")
        for _ in range(8):
            choice = argument("oneof", arguments=[argument("integer"), choice])
        before = normalize_snapshot(snapshot([argument("pure-token", token="ON")]))
        after = normalize_snapshot(snapshot([choice]))
        self.assertEqual(compare_commands(before, after, max_findings=0), [])
        with self.assertRaises(compatibility.WorkLimitError):
            compare_commands(before, after, max_findings=0, max_work=10)

    def test_work_budget_covers_sequence_alignment_and_metadata(self) -> None:
        cases = [
            (snapshot([argument(tag=i) for i in range(128)]), snapshot([argument(tag=i + 128) for i in range(128)])),
            (
                snapshot(reply_schema={"properties": {str(i): {"const": i} for i in range(100)}}),
                snapshot(reply_schema={"properties": {str(i): {"const": i + 1} for i in range(100)}}),
            ),
        ]
        for old, new in cases:
            with self.subTest(old=old), self.assertRaises(compatibility.WorkLimitError):
                compare_commands(normalize_snapshot(old), normalize_snapshot(new), max_work=100)

    def test_work_budget_resets_between_comparisons(self) -> None:
        normalized = normalize_snapshot(snapshot())
        with self.assertRaises(compatibility.WorkLimitError):
            compare_commands(normalized, normalized, max_work=0)
        self.assertEqual(compare_commands(normalized, normalized), [])

    def test_partial_normalization_collects_supported_identities_without_mutation(self) -> None:
        for invalid in (
            {"arity": 0},
            {"arity": 1, "arguments": [{"type": "string", "optional": "false"}]},
            {"arity": 1, "arguments": [{"type": "key", "key_spec_index": -1}]},
            {"arity": 1, "arguments": [{"type": "future", "arguments": []}]},
        ):
            files = {"src/commands/test.json": {"GET": {"arity": 2}, "BAD": invalid}}
            original = copy.deepcopy(files)
            exclusions: list[dict[str, str]] = []
            with self.subTest(invalid=invalid):
                normalized = normalize_snapshot(files, unanalyzable=exclusions)
                self.assertEqual(list(normalized), ["GET"])
                self.assertEqual(exclusions[0]["command"], "BAD")
                self.assertEqual(exclusions[0]["path"], "src/commands/test.json")
                self.assertTrue(exclusions[0]["reason"])
                self.assertEqual(files, original)

    def test_partial_normalization_preserves_nested_command_identity(self) -> None:
        files = {"commands.json": {"CLIENT": {"arity": -2, "subcommands": {"KILL": {"arity": 0}, "ID": {"arity": 2}}}}}
        exclusions: list[dict[str, str]] = []
        self.assertEqual(list(normalize_snapshot(files, unanalyzable=exclusions)), ["CLIENT", "CLIENT ID"])
        self.assertEqual(exclusions[0]["command"], "CLIENT KILL")

    def test_partial_normalization_does_not_hide_ambiguous_identities(self) -> None:
        cases: list[dict[str, Any]] = [
            {"a.json": {"GET": {"arity": 0}}, "b.json": {"get": {"arity": 2}}},
            {"a.json": {"KILL": {"arity": 0, "container": []}}},
            {"a.json": {"CLIENT": {"arity": 0, "subcommands": []}}},
            {"a.json": {"GET": None}},
        ]
        for files in cases:
            with self.subTest(files=files), self.assertRaises(ValueError):
                normalize_snapshot(files, unanalyzable=[])

    def test_duplicate_normalized_command_id_is_invalid(self) -> None:
        cases: list[dict[str, Any]] = [
            {"src/commands/a.json": {"get": {"arity": 2}, "GET": {"arity": 2}}},
            {"src/commands/a.json": {"KILL": {"container": "CLIENT", "arity": -3}, "CLIENT|KILL": {"arity": -3}}},
            {"src/commands/a.json": {"GET": {"arity": 2}}, "src/commands/b.json": {"GET": {"arity": 2}}},
        ]
        for invalid in cases:
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, "duplicate normalized"):
                normalize_snapshot(invalid)

    def test_malformed_snapshot_shapes_are_invalid(self) -> None:
        cases: list[Any] = [
            {"src/commands/a.json": []},
            {"src/commands/a.json": {}},
            {"src/commands/a.json": {"GET": None}},
            {"src/commands/a.json": {"GET": {"arity": True}}},
            {"src/commands/a.json": {"GET": {"arity": 0}}},
            {"src/commands/a.json": {"GET": {"arity": 2, "container": []}}},
            snapshot(command_flags="WRITE"),
            snapshot(acl_categories=[1]),
            snapshot(key_specs={}),
            snapshot(reply_schema=[]),
            snapshot(future=float("nan")),
            {1: {}},
            {"src/commands/a.json": {"|": {"arity": 1}}},
        ]
        for invalid in cases:
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                normalize_snapshot(invalid)

    def test_malformed_argument_shapes_are_invalid(self) -> None:
        cases: list[dict[str, Any]] = [
            {"type": "block"},
            {"type": "oneof", "arguments": []},
            {"type": "oneof", "arguments": {}},
            argument(optional="false"),
            argument(multiple=1),
            argument(multiple_token=None),
            argument(token=4),
            {"type": "pure-token"},
            argument("key", key_spec_index=-1),
            argument("key", key_spec_index=True),
            argument(arguments=[]),
            {"name": "missing-type"},
        ]
        for invalid in cases:
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                normalize_snapshot(snapshot([invalid]))

    def test_malformed_nested_commands_are_invalid(self) -> None:
        for children in [[], {"KILL": {"arity": -3, "container": "OTHER"}}]:
            with self.subTest(children=children), self.assertRaises(ValueError):
                normalize_snapshot({"src/commands/a.json": {"CLIENT": {"arity": -2, "subcommands": children}}})


if __name__ == "__main__":
    unittest.main()
