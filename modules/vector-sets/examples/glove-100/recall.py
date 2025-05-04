#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#

import h5py
import redis
import numpy as np
from tqdm import tqdm
import argparse

# Initialize Redis connection
redis_client = redis.Redis(host='localhost', port=6379, decode_responses=True, encoding='utf-8')

def get_redis_neighbors(query_vector, k):
    """Get nearest neighbors using Redis VSIM command"""
    args = ["VSIM", "glove_embeddings_bin", "VALUES", "100"]
    args.extend(map(str, query_vector))
    args.extend(["COUNT", str(k)])
    args.extend(["EF", 100])
    if False:
        print(args)
        exit(1)
    results = redis_client.execute_command(*args)
    return [int(res) for res in results]

def calculate_recall(ground_truth, predicted, k):
    """Calculate recall@k"""
    relevant = set(ground_truth[:k])
    retrieved = set(predicted[:k])
    return len(relevant.intersection(retrieved)) / len(relevant)

def main():
    parser = argparse.ArgumentParser(description='Evaluate Redis VSIM recall')
    parser.add_argument('--k', type=int, default=10, help='Number of neighbors to evaluate (default: 10)')
    parser.add_argument('--batch', type=int, default=100, help='Progress update frequency (default: 100)')
    args = parser.parse_args()

    k = args.k
    batch_size = args.batch

    with h5py.File('glove-100-angular.hdf5', 'r') as f:
        test_vectors = f['test'][:]
        ground_truth_neighbors = f['neighbors'][:]
        
        num_queries = len(test_vectors)
        recalls = []
        
        print(f"Evaluating recall@{k} for {num_queries} test queries...")
        
        for i in tqdm(range(num_queries)):
            try:
                # Get Redis results
                redis_neighbors = get_redis_neighbors(test_vectors[i], k)
                
                # Get ground truth for this query
                true_neighbors = ground_truth_neighbors[i]
                
                # Calculate recall
                recall = calculate_recall(true_neighbors, redis_neighbors, k)
                recalls.append(recall)
                
                if (i + 1) % batch_size == 0:
                    current_avg_recall = np.mean(recalls)
                    print(f"Current average recall@{k} after {i+1} queries: {current_avg_recall:.4f}")
                
            except Exception as e:
                print(f"Error processing query {i}: {str(e)}")
                continue
        
        final_recall = np.mean(recalls)
        print("\nFinal Results:")
        print(f"Average recall@{k}: {final_recall:.4f}")
        print(f"Total queries evaluated: {len(recalls)}")
        
        # Save detailed results
        with open(f'recall_evaluation_results_k{k}.txt', 'w') as f:
            f.write(f"Average recall@{k}: {final_recall:.4f}\n")
            f.write(f"Total queries evaluated: {len(recalls)}\n")
            f.write(f"Individual query recalls: {recalls}\n")

if __name__ == "__main__":
    main()
