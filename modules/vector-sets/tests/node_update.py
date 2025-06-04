from test import TestCase, generate_random_vector
import struct
import math
import random

class VectorUpdateAndClusters(TestCase):
   def getname(self):
       return "VADD vector update with cluster relocation"

   def estimated_runtime(self):
       return 2.0  # Should take around 2 seconds

   def generate_cluster_vector(self, base_vec, noise=0.1):
       """Generate a vector that's similar to base_vec with some noise."""
       vec = [x + random.gauss(0, noise) for x in base_vec]
       # Normalize
       norm = math.sqrt(sum(x*x for x in vec))
       return [x/norm for x in vec]

   def test(self):
       dim = 128
       vectors_per_cluster = 5000

       # Create two very different base vectors for our clusters
       cluster1_base = generate_random_vector(dim)
       cluster2_base = [-x for x in cluster1_base]  # Opposite direction

       # Add vectors from first cluster
       for i in range(vectors_per_cluster):
           vec = self.generate_cluster_vector(cluster1_base)
           vec_bytes = struct.pack(f'{dim}f', *vec)
           self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes,
                                    f'{self.test_key}:cluster1:{i}')

       # Add vectors from second cluster
       for i in range(vectors_per_cluster):
           vec = self.generate_cluster_vector(cluster2_base)
           vec_bytes = struct.pack(f'{dim}f', *vec)
           self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes,
                                    f'{self.test_key}:cluster2:{i}')

       # Pick a test vector from cluster1
       test_key = f'{self.test_key}:cluster1:0'

       # Verify it's in cluster1 using VSIM
       initial_vec = self.generate_cluster_vector(cluster1_base)
       results = self.redis.execute_command('VSIM', self.test_key, 'VALUES', dim,
                                          *[str(x) for x in initial_vec],
                                          'COUNT', 100, 'WITHSCORES')

       # Count how many cluster1 items are in top results
       cluster1_count = sum(1 for i in range(0, len(results), 2)
                          if b'cluster1' in results[i])
       assert cluster1_count > 80, "Initial clustering check failed"

       # Now update the test vector to be in cluster2
       new_vec = self.generate_cluster_vector(cluster2_base, noise=0.05)
       vec_bytes = struct.pack(f'{dim}f', *new_vec)
       self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, test_key)

       # Verify the embedding was actually updated using VEMB
       emb_result = self.redis.execute_command('VEMB', self.test_key, test_key)
       updated_vec = [float(x) for x in emb_result]

       # Verify updated vector matches what we inserted
       dot_product = sum(a*b for a,b in zip(updated_vec, new_vec))
       similarity = dot_product / (math.sqrt(sum(x*x for x in updated_vec)) *
                                 math.sqrt(sum(x*x for x in new_vec)))
       assert similarity > 0.9, "Vector was not properly updated"

       # Verify it's now in cluster2 using VSIM
       results = self.redis.execute_command('VSIM', self.test_key, 'VALUES', dim,
                                          *[str(x) for x in cluster2_base],
                                          'COUNT', 100, 'WITHSCORES')

       # Verify our updated vector is among top results
       found = False
       for i in range(0, len(results), 2):
           if results[i].decode() == test_key:
               found = True
               similarity = float(results[i+1])
               assert similarity > 0.80, f"Updated vector has low similarity: {similarity}"
               break

       assert found, "Updated vector not found in cluster2 proximity"
