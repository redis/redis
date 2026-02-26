#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#

import csv
import requests
import redis

ModelName="mxbai-embed-large"

# Initialize Redis connection, setting encoding to utf-8
redis_client = redis.Redis(host='localhost', port=6379, decode_responses=True, encoding='utf-8')

def get_embedding(text):
    """Get embedding from local API"""
    url = "http://localhost:11434/api/embeddings"
    payload = {
        "model": ModelName,
        "prompt": "Represent this movie plot and genre: "+text
    }
    response = requests.post(url, json=payload)
    return response.json()['embedding']

def add_to_redis(title, embedding, quant_type):
    """Add embedding to Redis using VADD command"""
    args = ["VADD", "many_movies_"+ModelName+"_"+quant_type, "VALUES", str(len(embedding))]
    args.extend(map(str, embedding))
    args.append(title)
    args.append(quant_type)
    redis_client.execute_command(*args)

def main():
    with open('mpst_full_data.csv', 'r', encoding='utf-8') as file:
        reader = csv.DictReader(file)

        for movie in reader:
            try:
                text_to_embed = f"{movie['title']} {movie['plot_synopsis']} {movie['tags']}"

                print(f"Getting embedding for: {movie['title']}")
                embedding = get_embedding(text_to_embed)

                add_to_redis(movie['title'], embedding, "BIN")
                add_to_redis(movie['title'], embedding, "NOQUANT")
                print(f"Successfully processed: {movie['title']}")

            except Exception as e:
                print(f"Error processing {movie['title']}: {str(e)}")
                continue

if __name__ == "__main__":
    main()
