"""Bloom COMMAND DOCS/INFO cases ported from test_docs_help.py for native metadata."""
from support import *

EXPECTED = {
    "bf.add": {
        "summary": "Add an item to a Bloom filter, creating it if needed.",
        "complexity": "O(k), where k is the number of hash functions across subfilters.",
        "arity": 3,
        "args": [
            [
                "key",
                "key"
            ],
            [
                "item",
                "string"
            ]
        ]
    },
    "bf.exists": {
        "summary": "Test whether an item may be present in a Bloom filter.",
        "complexity": "O(k), where k is the number of hash functions across subfilters.",
        "arity": 3,
        "args": [
            [
                "key",
                "key"
            ],
            [
                "item",
                "string"
            ]
        ]
    },
    "bf.madd": {
        "summary": "Adds one or more items to a Bloom filter.",
        "complexity": "O(k * n), where k is the number of hash functions across filters and n is the number of items.",
        "arity": -3,
        "args": [
            [
                "key",
                "key"
            ],
            [
                "item",
                "string"
            ]
        ]
    },
    "bf.mexists": {
        "summary": "Checks whether one or more items exist in a Bloom filter.",
        "complexity": "O(k * n), where k is the number of hash functions across filters and n is the number of items.",
        "arity": -3,
        "args": [
            [
                "key",
                "key"
            ],
            [
                "item",
                "string"
            ]
        ]
    },
    "bf.reserve": {
        "summary": "Create a Bloom filter.",
        "complexity": "O(1)",
        "arity": -4,
        "args": [
            [
                "key",
                "key"
            ],
            [
                "error-rate",
                "double"
            ],
            [
                "capacity",
                "integer"
            ],
            [
                "expansion",
                "integer"
            ],
            [
                "nonscaling",
                "pure-token"
            ]
        ]
    },
    "bf.insert": {
        "summary": "Adds items to a Bloom filter, optionally configuring its creation.",
        "complexity": "O(k * n), where k is the number of hash functions across filters and n is the number of items.",
        "arity": -4,
        "args": [
            [
                "key",
                "key"
            ],
            [
                "capacity",
                "integer"
            ],
            [
                "error",
                "double"
            ],
            [
                "expansion",
                "integer"
            ],
            [
                "nocreate",
                "pure-token"
            ],
            [
                "nonscaling",
                "pure-token"
            ],
            [
                "items",
                "pure-token"
            ],
            [
                "item",
                "string"
            ]
        ]
    },
    "bf.info": {
        "summary": "Returns information about a Bloom filter.",
        "complexity": "O(n), where n is the number of filters.",
        "arity": -2,
        "args": [
            [
                "key",
                "key"
            ],
            [
                "information",
                "oneof"
            ]
        ]
    },
    "bf.card": {
        "summary": "Returns the cardinality of a Bloom filter.",
        "complexity": "O(1)",
        "arity": 2,
        "args": [
            [
                "key",
                "key"
            ]
        ]
    },
    "bf.debug": {
        "summary": "Returns internal Bloom filter diagnostics.",
        "complexity": "O(n), where n is the number of filters.",
        "arity": 2,
        "args": [
            [
                "key",
                "key"
            ]
        ]
    },
    "bf.scandump": {
        "summary": "Serialize a Bloom filter incrementally.",
        "complexity": "O(n), where n is the chunk size.",
        "arity": 3,
        "args": [
            [
                "key",
                "key"
            ],
            [
                "iterator",
                "integer"
            ]
        ]
    },
    "bf.loadchunk": {
        "summary": "Restore a serialized Bloom filter chunk.",
        "complexity": "O(n), where n is the chunk size.",
        "arity": 4,
        "args": [
            [
                "key",
                "key"
            ],
            [
                "iterator",
                "integer"
            ],
            [
                "data",
                "string"
            ]
        ]
    }
}

def pairs(value):
    return value if isinstance(value, dict) else dict(zip(value[::2], value[1::2]))

class testCommandDocsAndHelp:
    def __init__(self):
        self.env = Env(decodeResponses=True)

def docs_case(command):
    def test(self):
        env = self.env
        expected = EXPECTED[command]
        docs = pairs(pairs(env.cmd("COMMAND", "DOCS", command))[command])
        env.assertEqual("bloom", docs["group"])
        env.assertNotIn("module", docs)
        env.assertEqual("8.8.0", docs["since"])
        env.assertEqual(expected["summary"], docs["summary"])
        env.assertEqual(expected["complexity"], docs["complexity"])
        arguments = [pairs(a) for a in docs["arguments"]]
        env.assertEqual(expected["args"], [[a["name"], a["type"]] for a in arguments])
        env.assertEqual(0, arguments[0]["key_spec_index"])
        info = env.cmd("COMMAND", "INFO", command)[0]
        env.assertEqual(expected["arity"], info[1])
        env.assertEqual([1, 1, 1], info[3:6])
        env.assertIn("@bloom", info[6])
        env.assertNotIn("module", info[2])
        key_spec = pairs(info[8][0])
        search = pairs(key_spec["begin_search"])
        env.assertEqual(1, pairs(search["spec"])["index"])
    return test

for command in EXPECTED:
    setattr(testCommandDocsAndHelp, "test_command_docs_" + command.replace(".", "_"), docs_case(command))
