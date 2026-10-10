#!/usr/bin/env python3
"""Run migrated Cuckoo flow tests and native integration tests, without pip deps."""
import importlib
import inspect
from pathlib import Path
import random
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bloom"))
from support import close_environments


def run_case(cls, name):
    random.seed(0)
    try:
        instance = cls()
        result = getattr(instance, name)()
        if inspect.isgenerator(result):
            # Upstream generator tests continue through SAVE/restart assertions.
            for _ in result:
                pass
    finally:
        close_environments()


suite = unittest.TestSuite()
filters = sys.argv[1:]
for path in sorted(Path(__file__).parent.glob("test_*.py")):
    module = importlib.import_module(path.stem)
    for name, cls in inspect.getmembers(module, inspect.isclass):
        if cls.__module__ != module.__name__ or not name.startswith("test"):
            continue
        for method in sorted(n for n in dir(cls) if n.startswith("test")):
            label = f"{path.stem}.{name}.{method}"
            if filters and not any(f in label for f in filters):
                continue
            suite.addTest(unittest.FunctionTestCase(
                lambda cls=cls, method=method: run_case(cls, method), description=label))
    for name, fn in inspect.getmembers(module, inspect.isfunction):
        label = f"{path.stem}.{name}"
        if fn.__module__ == module.__name__ and name.startswith("test_") and (not filters or any(f in label for f in filters)):
            def run_function(fn=fn):
                try:
                    fn()
                finally:
                    close_environments()
            suite.addTest(unittest.FunctionTestCase(run_function, description=label))
if suite.countTestCases() == 0:
    sys.exit("No Cuckoo tests match the requested filters")
result = unittest.TextTestRunner(verbosity=2).run(suite)
sys.exit(not result.wasSuccessful())
