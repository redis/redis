#!/usr/bin/env python3
#
# Vector set tests.
# A Redis instance should be running in the default port.
#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of the Redis Source Available License 2.0
# (RSALv2) or the Server Side Public License v1 (SSPLv1).
#

#!/usr/bin/env python3
import redis
import random
import struct
import math
import time
import sys
import os
import importlib
import inspect
from typing import List, Tuple, Optional
from dataclasses import dataclass

def colored(text: str, color: str) -> str:
    colors = {
        'red': '\033[91m',
        'green': '\033[92m'
    }
    reset = '\033[0m'
    return f"{colors.get(color, '')}{text}{reset}"

@dataclass
class VectorData:
    vectors: List[List[float]]
    names: List[str]

    def find_k_nearest(self, query_vector: List[float], k: int) -> List[Tuple[str, float]]:
        """Find k-nearest neighbors using the same scoring as Redis VSIM WITHSCORES."""
        similarities = []
        query_norm = math.sqrt(sum(x*x for x in query_vector))
        if query_norm == 0:
            return []

        for i, vec in enumerate(self.vectors):
            vec_norm = math.sqrt(sum(x*x for x in vec))
            if vec_norm == 0:
                continue

            dot_product = sum(a*b for a,b in zip(query_vector, vec))
            cosine_sim = dot_product / (query_norm * vec_norm)
            distance = 1.0 - cosine_sim
            redis_similarity = 1.0 - (distance/2.0)
            similarities.append((self.names[i], redis_similarity))

        similarities.sort(key=lambda x: x[1], reverse=True)
        return similarities[:k]

def generate_random_vector(dim: int) -> List[float]:
    """Generate a random normalized vector."""
    vec = [random.gauss(0, 1) for _ in range(dim)]
    norm = math.sqrt(sum(x*x for x in vec))
    return [x/norm for x in vec]

def fill_redis_with_vectors(r: redis.Redis, key: str, count: int, dim: int, 
                          with_reduce: Optional[int] = None) -> VectorData:
    """Fill Redis with random vectors and return a VectorData object for verification."""
    vectors = []
    names = []

    r.delete(key)
    for i in range(count):
        vec = generate_random_vector(dim)
        name = f"{key}:item:{i}"
        vectors.append(vec)
        names.append(name)

        vec_bytes = struct.pack(f'{dim}f', *vec)
        args = [key]
        if with_reduce:
            args.extend(['REDUCE', with_reduce])
        args.extend(['FP32', vec_bytes, name])
        r.execute_command('VADD', *args)

    return VectorData(vectors=vectors, names=names)

class TestCase:
    def __init__(self):
        self.error_msg = None
        self.error_details = None
        self.test_key = f"test:{self.__class__.__name__.lower()}"
        # Primary Redis instance (default port)
        self.redis = redis.Redis()
        # Replica Redis instance (port 6380)
        self.replica = redis.Redis(port=6380)
        # Replication status
        self.replication_setup = False

    def setup(self):
        self.redis.delete(self.test_key)

    def teardown(self):
        self.redis.delete(self.test_key)

    def setup_replication(self) -> bool:
        """
        Setup replication between primary and replica Redis instances.
        Returns True if replication is successfully established, False otherwise.
        """
        # Configure replica to replicate from primary
        self.replica.execute_command('REPLICAOF', '127.0.0.1', 6379)

        # Wait for replication to be established
        max_attempts = 10
        for attempt in range(max_attempts):
            # Check replication info
            repl_info = self.replica.info('replication')

            # Check if replication is established
            if (repl_info.get('role') == 'slave' and
                repl_info.get('master_host') == '127.0.0.1' and
                repl_info.get('master_port') == 6379 and
                repl_info.get('master_link_status') == 'up'):

                self.replication_setup = True
                return True

            # Wait before next attempt
            time.sleep(0.5)

        # If we get here, replication wasn't established
        self.error_msg = "Failed to establish replication between primary and replica"
        return False

    def test(self):
        raise NotImplementedError("Subclasses must implement test method")

    def run(self):
        try:
            self.setup()
            self.test()
            return True
        except AssertionError as e:
            self.error_msg = str(e)
            import traceback
            self.error_details = traceback.format_exc()
            return False
        except Exception as e:
            self.error_msg = f"Unexpected error: {str(e)}"
            import traceback
            self.error_details = traceback.format_exc()
            return False
        finally:
            self.teardown()

    def getname(self):
        """Each test class should override this to provide its name"""
        return self.__class__.__name__

    def estimated_runtime(self):
        """"Each test class should override this if it takes a significant amount of time to run. Default is 100ms"""
        return 0.1

def find_test_classes():
    test_classes = []
    tests_dir = 'tests'

    if not os.path.exists(tests_dir):
        return []

    for file in os.listdir(tests_dir):
        if file.endswith('.py'):
            module_name = f"tests.{file[:-3]}"
            try:
                module = importlib.import_module(module_name)
                for name, obj in inspect.getmembers(module):
                    if inspect.isclass(obj) and obj.__name__ != 'TestCase' and hasattr(obj, 'test'):
                        test_classes.append(obj())
            except Exception as e:
                print(f"Error loading {file}: {e}")

    return test_classes

def run_tests():
    print("================================================\n"+
          "Make sure to have Redis running in the localhost\n"+
          "with --enable-debug-command yes\n"+
          "Both primary (6379) and replica (6380) instances\n"+
          "================================================\n")

    tests = find_test_classes()
    if not tests:
        print("No tests found!")
        return

    # Sort tests by estimated runtime
    tests.sort(key=lambda t: t.estimated_runtime())

    passed = 0
    total = len(tests)

    for test in tests:
        print(f"{test.getname()}: ", end="")
        sys.stdout.flush()

        start_time = time.time()
        success = test.run()
        duration = time.time() - start_time

        if success:
            print(colored("OK", "green"), f"({duration:.2f}s)")
            passed += 1
        else:
            print(colored("ERR", "red"), f"({duration:.2f}s)")
            print(f"Error: {test.error_msg}")
            if test.error_details:
                print("\nTraceback:")
                print(test.error_details)

    print("\n" + "="*50)
    print(f"\nTest Summary: {passed}/{total} tests passed")

    if passed == total:
        print(colored("\nALL TESTS PASSED!", "green"))
        sys.exit(0)
    else:
        print(colored(f"\n{total-passed} TESTS FAILED!", "red"))
        sys.exit(1)

if __name__ == "__main__":
    run_tests()
