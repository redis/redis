from test import TestCase, generate_random_vector
import threading
import struct
import math
import time
import random
from typing import List, Dict

class ConcurrentCASTest(TestCase):
    def getname(self):
        return "Concurrent VADD with CAS"

    def estimated_runtime(self):
        return 1.5

    def worker(self, vectors: List[List[float]], start_idx: int, end_idx: int,
              dim: int, results: Dict[str, bool]):
        """Worker thread that adds a subset of vectors using VADD CAS"""
        for i in range(start_idx, end_idx):
            vec = vectors[i]
            name = f"{self.test_key}:item:{i}"
            vec_bytes = struct.pack(f'{dim}f', *vec)

            # Try to add the vector with CAS
            try:
                result = self.redis.execute_command('VADD', self.test_key, 'FP32',
                                                  vec_bytes, name, 'CAS')
                results[name] = (result == 1)  # Store if it was actually added
            except Exception as e:
                results[name] = False
                print(f"Error adding {name}: {e}")

    def verify_vector_similarity(self, vec1: List[float], vec2: List[float]) -> float:
        """Calculate cosine similarity between two vectors"""
        dot_product = sum(a*b for a,b in zip(vec1, vec2))
        norm1 = math.sqrt(sum(x*x for x in vec1))
        norm2 = math.sqrt(sum(x*x for x in vec2))
        return dot_product / (norm1 * norm2) if norm1 > 0 and norm2 > 0 else 0

    def test(self):
        # Test parameters
        dim = 128
        total_vectors = 5000
        num_threads = 8
        vectors_per_thread = total_vectors // num_threads

        # Generate all vectors upfront
        random.seed(42)  # For reproducibility
        vectors = [generate_random_vector(dim) for _ in range(total_vectors)]

        # Prepare threads and results dictionary
        threads = []
        results = {}  # Will store success/failure for each vector

        # Launch threads
        for i in range(num_threads):
            start_idx = i * vectors_per_thread
            end_idx = start_idx + vectors_per_thread if i < num_threads-1 else total_vectors
            thread = threading.Thread(target=self.worker,
                                   args=(vectors, start_idx, end_idx, dim, results))
            threads.append(thread)
            thread.start()

        # Wait for all threads to complete
        for thread in threads:
            thread.join()

        # Verify cardinality
        card = self.redis.execute_command('VCARD', self.test_key)
        assert card == total_vectors, \
            f"Expected {total_vectors} elements, but found {card}"

        # Verify each vector
        num_verified = 0
        for i in range(total_vectors):
            name = f"{self.test_key}:item:{i}"

            # Verify the item was successfully added
            assert results[name], f"Vector {name} was not successfully added"

            # Get the stored vector
            stored_vec_raw = self.redis.execute_command('VEMB', self.test_key, name)
            stored_vec = [float(x) for x in stored_vec_raw]

            # Verify vector dimensions
            assert len(stored_vec) == dim, \
                f"Stored vector dimension mismatch for {name}: {len(stored_vec)} != {dim}"

            # Calculate similarity with original vector
            similarity = self.verify_vector_similarity(vectors[i], stored_vec)
            assert similarity > 0.99, \
                f"Low similarity ({similarity}) for {name}"

            num_verified += 1

        # Final verification
        assert num_verified == total_vectors, \
            f"Only verified {num_verified} out of {total_vectors} vectors"
