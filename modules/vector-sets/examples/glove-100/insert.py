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
from tqdm import tqdm

# Initialize Redis connection
redis_client = redis.Redis(host='localhost', port=6379, decode_responses=True, encoding='utf-8')

def add_to_redis(index, embedding):
    """Add embedding to Redis using VADD command"""
    args = ["VADD", "glove_embeddings", "VALUES", "100"]  # 100 is vector dimension
    args.extend(map(str, embedding))
    args.append(f"{index}")  # Using index as identifier since we don't have words
    args.append("EF")
    args.append("200")
    # args.append("NOQUANT")
    # args.append("BIN")
    redis_client.execute_command(*args)

def main():
    with h5py.File('glove-100-angular.hdf5', 'r') as f:
        # Get the train dataset
        train_vectors = f['train']
        total_vectors = train_vectors.shape[0]

        print(f"Starting to process {total_vectors} vectors...")

        # Process in batches to avoid memory issues
        batch_size = 1000

        for i in tqdm(range(0, total_vectors, batch_size)):
            batch_end = min(i + batch_size, total_vectors)
            batch = train_vectors[i:batch_end]

            for j, vector in enumerate(batch):
                try:
                    current_index = i + j
                    add_to_redis(current_index, vector)

                except Exception as e:
                    print(f"Error processing vector {current_index}: {str(e)}")
                    continue

            if (i + batch_size) % 10000 == 0:
                print(f"Processed {i + batch_size} vectors")

if __name__ == "__main__":
    main()
