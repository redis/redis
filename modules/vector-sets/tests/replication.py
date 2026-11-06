from test import TestCase, generate_random_vector
import struct
import random
import time

class ComprehensiveReplicationTest(TestCase):
    def getname(self):
        return "Comprehensive Replication Test with mixed operations"

    def estimated_runtime(self):
        # This test will take longer than the default 100ms
        return 20.0  # 20 seconds estimate

    def test(self):
        # Setup replication between primary and replica
        assert self.setup_replication(), "Failed to setup replication"

        # Test parameters
        num_vectors = 5000
        vector_dim = 8
        delete_probability = 0.1
        cas_probability = 0.3

        # Keep track of added items for potential deletion
        added_items = []

        # Add vectors and occasionally delete
        for i in range(num_vectors):
            # Generate a random vector
            vec = generate_random_vector(vector_dim)
            vec_bytes = struct.pack(f'{vector_dim}f', *vec)
            item_name = f"{self.test_key}:item:{i}"

            # Decide whether to use CAS or not
            use_cas = random.random() < cas_probability

            if use_cas and added_items:
                # Get an existing item for CAS reference (if available)
                cas_item = random.choice(added_items)
                try:
                    # Add with CAS
                    result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes,
                                                   item_name, 'CAS')
                    # Only add to our list if actually added (CAS might fail)
                    if result == 1:
                        added_items.append(item_name)
                except Exception as e:
                    print(f"  CAS VADD failed: {e}")
            else:
                try:
                    # Add without CAS
                    result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, item_name)
                    # Only add to our list if actually added
                    if result == 1:
                        added_items.append(item_name)
                except Exception as e:
                    print(f"  VADD failed: {e}")

            # Randomly delete items (with 10% probability)
            if random.random() < delete_probability and added_items:
                try:
                    # Select a random item to delete
                    item_to_delete = random.choice(added_items)
                    # Delete the item using VREM (not VDEL)
                    self.redis.execute_command('VREM', self.test_key, item_to_delete)
                    # Remove from our list
                    added_items.remove(item_to_delete)
                except Exception as e:
                    print(f"  VREM failed: {e}")

        # Allow time for replication to complete
        time.sleep(2.0)

        # Verify final VCARD matches
        primary_card = self.redis.execute_command('VCARD', self.test_key)
        replica_card = self.replica.execute_command('VCARD', self.test_key)
        assert primary_card == replica_card, f"Final VCARD mismatch: primary={primary_card}, replica={replica_card}"

        # Verify VDIM matches
        primary_dim = self.redis.execute_command('VDIM', self.test_key)
        replica_dim = self.replica.execute_command('VDIM', self.test_key)
        assert primary_dim == replica_dim, f"VDIM mismatch: primary={primary_dim}, replica={replica_dim}"

        # Verify digests match using DEBUG DIGEST
        primary_digest = self.redis.execute_command('DEBUG', 'DIGEST-VALUE', self.test_key)
        replica_digest = self.replica.execute_command('DEBUG', 'DIGEST-VALUE', self.test_key)
        assert primary_digest == replica_digest, f"Digest mismatch: primary={primary_digest}, replica={replica_digest}"

        # Print summary
        print(f"\n  Added and maintained {len(added_items)} vectors with dimension {vector_dim}")
        print(f"  Final vector count: {primary_card}")
        print(f"  Final digest: {primary_digest[0].decode()}")
