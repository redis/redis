from test import TestCase, fill_redis_with_vectors, generate_random_vector
import random

"""
A note about this test:
It was experimentally tried to modify hnsw.c in order to
avoid calling hnsw_reconnect_nodes(). In this case, the test
fails very often with EF set to 250, while it hardly
fails at all with the same parameters if hnsw_reconnect_nodes()
is called.

Note that for the nature of the test (it is very strict) it can
still fail from time to time, without this signaling any
actual bug.
"""

class VREM(TestCase):
    def getname(self):
        return "Deletion and graph state after deletion"

    def estimated_runtime(self):
        return 2.0

    def format_neighbors_with_scores(self, links_result, old_links=None, items_to_remove=None):
        """Format neighbors with their similarity scores and status indicators"""
        if not links_result:
            return "No neighbors"

        output = []
        for level, neighbors in enumerate(links_result):
            level_num = len(links_result) - level - 1
            output.append(f"Level {level_num}:")

            # Get neighbors and scores
            neighbors_with_scores = []
            for i in range(0, len(neighbors), 2):
                neighbor = neighbors[i].decode() if isinstance(neighbors[i], bytes) else neighbors[i]
                score = float(neighbors[i+1]) if i+1 < len(neighbors) else None
                status = ""

                # For old links, mark deleted ones
                if items_to_remove and neighbor in items_to_remove:
                    status = " [lost]"
                # For new links, mark newly added ones
                elif old_links is not None:
                    # Check if this neighbor was in the old links at this level
                    was_present = False
                    if old_links and level < len(old_links):
                        old_neighbors = [n.decode() if isinstance(n, bytes) else n
                                      for n in old_links[level]]
                        was_present = neighbor in old_neighbors
                    if not was_present:
                        status = " [gained]"

                if score is not None:
                    neighbors_with_scores.append(f"{len(neighbors_with_scores)+1}. {neighbor} ({score:.6f}){status}")
                else:
                    neighbors_with_scores.append(f"{len(neighbors_with_scores)+1}. {neighbor}{status}")

            output.extend(["    " + n for n in neighbors_with_scores])
        return "\n".join(output)

    def test(self):
        # 1. Fill server with random elements
        dim = 128
        count = 5000
        data = fill_redis_with_vectors(self.redis, self.test_key, count, dim)

        # 2. Do VSIM to get 200 items
        query_vec = generate_random_vector(dim)
        results = self.redis.execute_command('VSIM', self.test_key, 'VALUES', dim,
                                    *[str(x) for x in query_vec],
                                    'COUNT', 200, 'WITHSCORES')

        # Convert results to list of (item, score) pairs, sorted by score
        items = []
        for i in range(0, len(results), 2):
            item = results[i].decode()
            score = float(results[i+1])
            items.append((item, score))
        items.sort(key=lambda x: x[1], reverse=True)  # Sort by similarity

        # Store the graph structure for all items before deletion
        neighbors_before = {}
        for item, _ in items:
            links = self.redis.execute_command('VLINKS', self.test_key, item, 'WITHSCORES')
            if links:  # Some items might not have links
                neighbors_before[item] = links

        # 3. Remove 100 random items
        items_to_remove = set(item for item, _ in random.sample(items, 100))
        # Keep track of top 10 non-removed items
        top_remaining = []
        for item, score in items:
            if item not in items_to_remove:
                top_remaining.append((item, score))
                if len(top_remaining) == 10:
                    break

        # Remove the items
        for item in items_to_remove:
            result = self.redis.execute_command('VREM', self.test_key, item)
            assert result == 1, f"VREM failed to remove {item}"

        # 4. Do VSIM again with same vector
        new_results = self.redis.execute_command('VSIM', self.test_key, 'VALUES', dim,
                                        *[str(x) for x in query_vec],
                                        'COUNT', 200, 'WITHSCORES',
                                        'EF', 500)

        # Convert new results to dict of item -> score
        new_scores = {}
        for i in range(0, len(new_results), 2):
            item = new_results[i].decode()
            score = float(new_results[i+1])
            new_scores[item] = score

        failure = False
        failed_item = None
        failed_reason = None
        # 5. Verify all top 10 non-removed items are still found with similar scores
        for item, old_score in top_remaining:
            if item not in new_scores:
                failure = True
                failed_item = item
                failed_reason = "missing"
                break
            new_score = new_scores[item]
            if abs(new_score - old_score) >= 0.01:
                failure = True
                failed_item = item
                failed_reason = f"score changed: {old_score:.6f} -> {new_score:.6f}"
                break

        if failure:
            print("\nTest failed!")
            print(f"Problem with item: {failed_item} ({failed_reason})")

            print("\nOriginal neighbors (with similarity scores):")
            if failed_item in neighbors_before:
                print(self.format_neighbors_with_scores(
                    neighbors_before[failed_item], 
                    items_to_remove=items_to_remove))
            else:
                print("No neighbors found in original graph")

            print("\nCurrent neighbors (with similarity scores):")
            current_links = self.redis.execute_command('VLINKS', self.test_key, 
                                                     failed_item, 'WITHSCORES')
            if current_links:
                print(self.format_neighbors_with_scores(
                    current_links,
                    old_links=neighbors_before.get(failed_item)))
            else:
                print("No neighbors in current graph")

            print("\nOriginal results (top 20):")
            for item, score in items[:20]:
                deleted = "[deleted]" if item in items_to_remove else ""
                print(f"{item}: {score:.6f} {deleted}")

            print("\nNew results after removal (top 20):")
            new_items = []
            for i in range(0, len(new_results), 2):
                item = new_results[i].decode()
                score = float(new_results[i+1])
                new_items.append((item, score))
            new_items.sort(key=lambda x: x[1], reverse=True)
            for item, score in new_items[:20]:
                print(f"{item}: {score:.6f}")

            raise AssertionError(f"Test failed: Problem with item {failed_item} ({failed_reason}). *** IMPORTANT *** This test may fail from time to time without indicating that there is a bug. However normally it should pass. The fact is that it's a quite extreme test where we destroy 50% of nodes of top results and still expect perfect recall, with vectors that are very hostile because of the distribution used.")

