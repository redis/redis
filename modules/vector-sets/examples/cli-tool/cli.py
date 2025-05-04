#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#

#!/usr/bin/env python3
import redis
import requests
import re
import shlex
from prompt_toolkit import PromptSession
from prompt_toolkit.history import InMemoryHistory

def get_embedding(text):
    """Get embedding from local Ollama API"""
    url = "http://localhost:11434/api/embeddings"
    payload = {
        "model": "mxbai-embed-large",
        "prompt": text
    }
    try:
        response = requests.post(url, json=payload)
        response.raise_for_status()
        return response.json()['embedding']
    except requests.exceptions.RequestException as e:
        raise Exception(f"Failed to get embedding: {str(e)}")

def process_embedding_patterns(text):
    """Process !"text" and !!"text" patterns in the command"""

    def replace_with_embedding(match):
        text = match.group(1)
        embedding = get_embedding(text)
        return f"VALUES {len(embedding)} {' '.join(map(str, embedding))}"

    def replace_with_embedding_and_text(match):
        text = match.group(1)
        embedding = get_embedding(text)
        # Return both the embedding values and the original text as next argument
        return f'VALUES {len(embedding)} {" ".join(map(str, embedding))} "{text}"'

    # First handle !!"text" pattern (must be done before !"text")
    text = re.sub(r'!!"([^"]*)"', replace_with_embedding_and_text, text)
    # Then handle !"text" pattern
    text = re.sub(r'!"([^"]*)"', replace_with_embedding, text)
    return text

def parse_command(command):
    """Parse command respecting quoted strings"""
    try:
        # Use shlex to properly handle quoted strings
        return shlex.split(command)
    except ValueError as e:
        raise Exception(f"Invalid command syntax: {str(e)}")

def format_response(response):
    """Format the response to match Redis protocol style"""
    if response is None:
        return "(nil)"
    elif isinstance(response, bool):
        return "+OK" if response else "(error) Operation failed"
    elif isinstance(response, (list, set)):
        if not response:
            return "(empty list or set)"
        return "\n".join(f"{i+1}) {item}" for i, item in enumerate(response))
    elif isinstance(response, int):
        return f"(integer) {response}"
    else:
        return str(response)

def main():
    # Default connection to localhost:6379
    r = redis.Redis(host='localhost', port=6379, decode_responses=True)

    try:
        # Test connection
        r.ping()
        print("Connected to Redis. Type your commands (CTRL+D to exit):")
        print("Special syntax:")
        print("  !\"text\"  - Replace with embedding")
        print("  !!\"text\" - Replace with embedding and append text as value")
        print("  \"text\"   - Quote strings containing spaces")
    except redis.ConnectionError:
        print("Error: Could not connect to Redis server")
        return

    # Setup prompt session with history
    session = PromptSession(history=InMemoryHistory())

    # Main loop
    while True:
        try:
            # Read input with line editing support
            command = session.prompt("redis> ")

            # Skip empty commands
            if not command.strip():
                continue

            # Process any embedding patterns before parsing
            try:
                processed_command = process_embedding_patterns(command)
            except Exception as e:
                print(f"(error) Embedding processing failed: {str(e)}")
                continue

            # Parse the command respecting quoted strings
            try:
                parts = parse_command(processed_command)
            except Exception as e:
                print(f"(error) {str(e)}")
                continue

            if not parts:
                continue

            cmd = parts[0].lower()
            args = parts[1:]

            # Execute command
            try:
                method = getattr(r, cmd, None)
                if method is not None:
                    result = method(*args)
                else:
                    # Use execute_command for unknown commands
                    result = r.execute_command(cmd, *args)
                print(format_response(result))
            except AttributeError:
                print(f"(error) Unknown command '{cmd}'")

        except EOFError:
            print("\nGoodbye!")
            break
        except KeyboardInterrupt:
            continue  # Allow Ctrl+C to clear current line
        except redis.RedisError as e:
            print(f"(error) {str(e)}")
        except Exception as e:
            print(f"(error) {str(e)}")

if __name__ == "__main__":
    main()
