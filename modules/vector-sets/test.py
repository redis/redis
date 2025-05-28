#!/usr/bin/env python3
#
# Vector set tests.
# A Redis instance should be running in the default port.
#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#

import redis
import random
import struct
import math
import time
import sys
import os
import importlib
import inspect
import argparse
from typing import List, Tuple, Optional
from dataclasses import dataclass

def colored(text: str, color: str) -> str:
    colors = {
        'red': '\033[91m',
        'green': '\033[92m',
        'yellow': '\033[93m',
        'blue': '\033[94m',
        'magenta': '\033[95m',
        'cyan': '\033[96m',
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
    def __init__(self, primary_port=6379, replica_port=6380):
        self.error_msg = None
        self.error_details = None
        self.test_key = f"test:{self.__class__.__name__.lower()}"
        # Primary Redis instance
        self.redis = redis.Redis(port=primary_port,db=9)
        self.redis3 = redis.Redis(port=primary_port,protocol=3,db=9)
        # Replica Redis instance
        self.replica = redis.Redis(port=replica_port,db=9)
        # Replication status
        self.replication_setup = False
        # Ports
        self.primary_port = primary_port
        self.replica_port = replica_port

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
        self.replica.execute_command('REPLICAOF', '127.0.0.1', self.primary_port)

        # Wait for replication to be established
        max_attempts = 50
        for attempt in range(max_attempts):
            # Check replication info
            repl_info = self.replica.info('replication')

            # Check if replication is established
            if (repl_info.get('role') == 'slave' and
                repl_info.get('master_host') == '127.0.0.1' and
                repl_info.get('master_port') == self.primary_port and
                repl_info.get('master_link_status') == 'up'):

                self.replication_setup = True
                return True

            # Wait before next attempt
            print(colored(".",'cyan'),end="",flush=True)
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

def find_test_classes(primary_port, replica_port):
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
                        # Create test instance with specified ports
                        test_instance = obj(primary_port,replica_port)
                        test_classes.append(test_instance)
            except Exception as e:
                print(f"Error loading {file}: {e}")

    return test_classes

def check_redis_empty(r, instance_name):
    """Check if Redis instance is empty"""
    try:
        dbsize = r.dbsize()
        if dbsize > 0:
            print(colored(f"ERROR: {instance_name} Redis instance DB 9 is not empty (dbsize: {dbsize}).", "red"))
            print(colored("Make sure you're not using a production instance and that all data is safe to delete.", "red"))
            sys.exit(1)
    except redis.exceptions.ConnectionError:
        print(colored(f"ERROR: Cannot connect to {instance_name} Redis instance.", "red"))
        sys.exit(1)

def check_replica_running(replica_port):
    """Check if replica Redis instance is running"""
    r = redis.Redis(port=replica_port)
    try:
        r.ping()
        return True
    except redis.exceptions.ConnectionError:
        print(colored(f"WARNING: Replica Redis instance (port {replica_port}) is not running.", "yellow"))
        print(colored("Replication tests will be skipped. Make sure to start the replica instance.", "yellow"))
        return False

def run_tests():
    # Parse command line arguments
    parser = argparse.ArgumentParser(description='Run Redis vector tests.')
    parser.add_argument('--primary-port', type=int, default=6379, help='Primary Redis instance port (default: 6379)')
    parser.add_argument('--replica-port', type=int, default=6380, help='Replica Redis instance port (default: 6380)')
    args = parser.parse_args()

    print("================================================")
    print(f"Make sure to have Redis running on localhost")
    print(f"Primary port: {args.primary_port}")
    print(f"Replica port: {args.replica_port}")
    print("with --enable-debug-command yes")
    print("================================================\n")

    # Check if Redis instances are empty
    primary = redis.Redis(port=args.primary_port,db=9)
    replica = redis.Redis(port=args.replica_port,db=9)

    check_redis_empty(primary, "Primary")

    # Check if replica is running
    replica_running = check_replica_running(args.replica_port)
    if replica_running:
        check_redis_empty(replica, "Replica")

    tests = find_test_classes(args.primary_port, args.replica_port)
    if not tests:
        print("No tests found!")
        return

    # Sort tests by estimated runtime
    tests.sort(key=lambda t: t.estimated_runtime())

    passed = 0
    skipped = 0
    total = len(tests)

    for test in tests:
        print(f"{test.getname()}: ", end="")
        sys.stdout.flush()

        if not replica_running and test.getname().lower().find("replication") != -1:
            print(colored("SKIPPING","yellow"))
            skipped += 1
            continue

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
        print(colored("ALL TESTS PASSED!", "green"))
    else:
        if total-skipped-passed > 0:
            print(colored(f"{total-skipped-passed} TESTS FAILED!", "red"))
        if skipped > 0:
            print(colored(f"{skipped} TESTS SKIPPED!", "yellow"))

if __name__ == "__main__":
    run_tests()
