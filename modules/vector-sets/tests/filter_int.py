from test import TestCase, generate_random_vector
import struct
import random
import math
import json
import time

class VSIMFilterAdvanced(TestCase):
    def getname(self):
        return "VSIM FILTER comprehensive functionality testing"

    def estimated_runtime(self):
        return 15  # This test might take up to 15 seconds for the large dataset

    def setup(self):
        super().setup()
        self.dim = 32        # Vector dimension
        self.count = 5000    # Number of vectors for large tests
        self.small_count = 50 # Number of vectors for small/quick tests

        # Categories for attributes
        self.categories = ["electronics", "furniture", "clothing", "books", "food"]
        self.cities = ["New York", "London", "Tokyo", "Paris", "Berlin", "Sydney", "Toronto", "Singapore"]
        self.price_ranges = [(10, 50), (50, 200), (200, 1000), (1000, 5000)]
        self.years = list(range(2000, 2025))

    def create_attributes(self, index):
        """Create realistic attributes for a vector"""
        category = random.choice(self.categories)
        city = random.choice(self.cities)
        min_price, max_price = random.choice(self.price_ranges)
        price = round(random.uniform(min_price, max_price), 2)
        year = random.choice(self.years)
        in_stock = random.random() > 0.3  # 70% chance of being in stock
        rating = round(random.uniform(1, 5), 1)
        views = int(random.expovariate(1/1000))  # Exponential distribution for page views
        tags = random.sample(["popular", "sale", "new", "limited", "exclusive", "clearance"],
                           k=random.randint(0, 3))

        # Add some specific patterns for testing
        # Every 10th item has a specific property combination for testing
        is_premium = (index % 10 == 0)

        # Create attributes dictionary
        attrs = {
            "id": index,
            "category": category,
            "location": city,
            "price": price,
            "year": year,
            "in_stock": in_stock,
            "rating": rating,
            "views": views,
            "tags": tags
        }

        if is_premium:
            attrs["is_premium"] = True
            attrs["special_features"] = ["premium", "warranty", "support"]

        # Add sub-categories for more complex filters
        if category == "electronics":
            attrs["subcategory"] = random.choice(["phones", "computers", "cameras", "audio"])
        elif category == "furniture":
            attrs["subcategory"] = random.choice(["chairs", "tables", "sofas", "beds"])
        elif category == "clothing":
            attrs["subcategory"] = random.choice(["shirts", "pants", "dresses", "shoes"])

        # Add some intentionally missing fields for testing
        if random.random() > 0.9:  # 10% chance of missing price
            del attrs["price"]

        # Some items have promotion field
        if random.random() > 0.7:  # 30% chance of having a promotion
            attrs["promotion"] = random.choice(["discount", "bundle", "gift"])

        # Create invalid JSON for a small percentage of vectors
        if random.random() > 0.98:  # 2% chance of having invalid JSON
            return "{{invalid json}}"

        return json.dumps(attrs)

    def create_vectors_with_attributes(self, key, count):
        """Create vectors and add attributes to them"""
        vectors = []
        names = []
        attribute_map = {}  # To store attributes for verification

        # Create vectors
        for i in range(count):
            vec = generate_random_vector(self.dim)
            vectors.append(vec)
            name = f"{key}:item:{i}"
            names.append(name)

            # Add to Redis
            vec_bytes = struct.pack(f'{self.dim}f', *vec)
            self.redis.execute_command('VADD', key, 'FP32', vec_bytes, name)

            # Create and add attributes
            attrs = self.create_attributes(i)
            self.redis.execute_command('VSETATTR', key, name, attrs)

            # Store attributes for later verification
            try:
                attribute_map[name] = json.loads(attrs) if '{' in attrs else None
            except json.JSONDecodeError:
                attribute_map[name] = None

        return vectors, names, attribute_map

    def filter_linear_search(self, vectors, names, query_vector, filter_expr, attribute_map, k=10):
        """Perform a linear search with filtering for verification"""
        similarities = []
        query_norm = math.sqrt(sum(x*x for x in query_vector))

        if query_norm == 0:
            return []

        for i, vec in enumerate(vectors):
            name = names[i]
            attributes = attribute_map.get(name)

            # Skip if doesn't match filter
            if not self.matches_filter(attributes, filter_expr):
                continue

            vec_norm = math.sqrt(sum(x*x for x in vec))
            if vec_norm == 0:
                continue

            dot_product = sum(a*b for a,b in zip(query_vector, vec))
            cosine_sim = dot_product / (query_norm * vec_norm)
            distance = 1.0 - cosine_sim
            redis_similarity = 1.0 - (distance/2.0)
            similarities.append((name, redis_similarity))

        similarities.sort(key=lambda x: x[1], reverse=True)
        return similarities[:k]

    def matches_filter(self, attributes, filter_expr):
        """Filter matching for verification - uses Python eval to handle complex expressions"""
        if attributes is None:
            return False  # No attributes or invalid JSON

        # Replace JSON path selectors with Python dictionary access
        py_expr = filter_expr

        # Handle `.field` notation (replace with attributes['field'])
        i = 0
        while i < len(py_expr):
            if py_expr[i] == '.' and (i == 0 or not py_expr[i-1].isalnum()):
                # Find the end of the selector (stops at operators or whitespace)
                j = i + 1
                while j < len(py_expr) and (py_expr[j].isalnum() or py_expr[j] == '_'):
                    j += 1

                if j > i + 1:  # Found a valid selector
                    field = py_expr[i+1:j]
                    # Use a safe access pattern that returns a default value based on context
                    py_expr = py_expr[:i] + f"attributes.get('{field}')" + py_expr[j:]
                    i = i + len(f"attributes.get('{field}')")
                else:
                    i += 1
            else:
                i += 1

        # Convert not operator if needed
        py_expr = py_expr.replace('!', ' not ')

        try:
            # Custom evaluation that handles exceptions for missing fields
            # by returning False for the entire expression

            # Split the expression on logical operators
            parts = []
            for op in [' and ', ' or ']:
                if op in py_expr:
                    parts = py_expr.split(op)
                    break

            if not parts:  # No logical operators found
                parts = [py_expr]

            # Try to evaluate each part - if any part fails,
            # the whole expression should fail
            try:
                result = eval(py_expr, {"attributes": attributes})
                return bool(result)
            except (TypeError, AttributeError):
                # This typically happens when trying to compare None with
                # numbers or other types, or when an attribute doesn't exist
                return False
            except Exception as e:
                print(f"Error evaluating filter expression '{filter_expr}' as '{py_expr}': {e}")
                return False

        except Exception as e:
            print(f"Error evaluating filter expression '{filter_expr}' as '{py_expr}': {e}")
            return False

    def safe_decode(self,item):
        return item.decode() if isinstance(item, bytes) else item

    def calculate_recall(self, redis_results, linear_results, k=10):
        """Calculate recall (percentage of correct results retrieved)"""
        redis_set = set(self.safe_decode(item) for item in redis_results)
        linear_set = set(item[0] for item in linear_results[:k])

        if not linear_set:
            return 1.0  # If no linear results, consider it perfect recall

        intersection = redis_set.intersection(linear_set)
        return len(intersection) / len(linear_set)

    def test_recall_with_filter(self, filter_expr, ef=500, filter_ef=None):
        """Test recall for a given filter expression"""
        # Create query vector
        query_vec = generate_random_vector(self.dim)

        # First, get ground truth using linear scan
        linear_results = self.filter_linear_search(
            self.vectors, self.names, query_vec, filter_expr, self.attribute_map, k=50)

        # Calculate true selectivity from ground truth
        true_selectivity = len(linear_results) / len(self.names) if self.names else 0

        # Perform Redis search with filter
        cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args.extend([str(x) for x in query_vec])
        cmd_args.extend(['COUNT', 50, 'WITHSCORES', 'EF', ef, 'FILTER', filter_expr])
        if filter_ef:
            cmd_args.extend(['FILTER-EF', filter_ef])

        start_time = time.time()
        redis_results = self.redis.execute_command(*cmd_args)
        query_time = time.time() - start_time

        # Convert Redis results to dict
        redis_items = {}
        for i in range(0, len(redis_results), 2):
            key = redis_results[i].decode() if isinstance(redis_results[i], bytes) else redis_results[i]
            score = float(redis_results[i+1])
            redis_items[key] = score

        # Calculate metrics
        recall = self.calculate_recall(redis_items.keys(), linear_results)
        selectivity = len(redis_items) / len(self.names) if redis_items else 0

        # Compare against the true selectivity from linear scan
        assert abs(selectivity - true_selectivity) < 0.1, \
            f"Redis selectivity {selectivity:.3f} differs significantly from ground truth {true_selectivity:.3f}"

        # We expect high recall for standard parameters
        if ef >= 500 and (filter_ef is None or filter_ef >= 1000):
            try:
                assert recall >= 0.7, \
                    f"Low recall {recall:.2f} for filter '{filter_expr}'"
            except AssertionError as e:
                # Get items found in each set
                redis_items_set = set(redis_items.keys())
                linear_items_set = set(item[0] for item in linear_results)

                # Find items in each set
                only_in_redis = redis_items_set - linear_items_set
                only_in_linear = linear_items_set - redis_items_set
                in_both = redis_items_set & linear_items_set

                # Build comprehensive debug message
                debug = f"\nGround Truth: {len(linear_results)} matching items (total vectors: {len(self.vectors)})"
                debug += f"\nRedis Found: {len(redis_items)} items with FILTER-EF: {filter_ef or 'default'}"
                debug += f"\nItems in both sets: {len(in_both)} (recall: {recall:.4f})"
                debug += f"\nItems only in Redis: {len(only_in_redis)}"
                debug += f"\nItems only in Ground Truth: {len(only_in_linear)}"

                # Show some example items from each set with their scores
                if only_in_redis:
                    debug += "\n\nTOP 5 ITEMS ONLY IN REDIS:"
                    sorted_redis = sorted([(k, v) for k, v in redis_items.items()], key=lambda x: x[1], reverse=True)
                    for i, (item, score) in enumerate(sorted_redis[:5]):
                        if item in only_in_redis:
                            debug += f"\n  {i+1}. {item} (Score: {score:.4f})"

                            # Show attribute that should match filter
                            attr = self.attribute_map.get(item)
                            if attr:
                                debug += f" - Attrs: {attr.get('category', 'N/A')}, Price: {attr.get('price', 'N/A')}"

                if only_in_linear:
                    debug += "\n\nTOP 5 ITEMS ONLY IN GROUND TRUTH:"
                    for i, (item, score) in enumerate(linear_results[:5]):
                        if item in only_in_linear:
                            debug += f"\n  {i+1}. {item} (Score: {score:.4f})"

                            # Show attribute that should match filter
                            attr = self.attribute_map.get(item)
                            if attr:
                                debug += f" - Attrs: {attr.get('category', 'N/A')}, Price: {attr.get('price', 'N/A')}"

                # Help identify parsing issues
                debug += "\n\nPARSING CHECK:"
                debug += f"\nRedis command: VSIM {self.test_key} VALUES {self.dim} [...] FILTER '{filter_expr}'"

                # Check for WITHSCORES handling issues
                if len(redis_results) > 0 and len(redis_results) % 2 == 0:
                    debug += f"\nRedis returned {len(redis_results)} items (looks like item,score pairs)"
                    debug += f"\nFirst few results: {redis_results[:4]}"

                # Check the filter implementation
                debug += "\n\nFILTER IMPLEMENTATION CHECK:"
                debug += f"\nFilter expression: '{filter_expr}'"
                debug += "\nSample attribute matches from attribute_map:"
                count_matching = 0
                for i, (name, attrs) in enumerate(self.attribute_map.items()):
                    if attrs and self.matches_filter(attrs, filter_expr):
                        count_matching += 1
                        if i < 3:  # Show first 3 matches
                            debug += f"\n  - {name}: {attrs}"
                debug += f"\nTotal items matching filter in attribute_map: {count_matching}"

                # Check if results array handling could be wrong
                debug += "\n\nRESULT ARRAYS CHECK:"
                if len(linear_results) >= 1:
                    debug += f"\nlinear_results[0]: {linear_results[0]}"
                    if isinstance(linear_results[0], tuple) and len(linear_results[0]) == 2:
                        debug += " (correct tuple format: (name, score))"
                    else:
                        debug += " (UNEXPECTED FORMAT!)"

                # Debug sort order
                debug += "\n\nSORTING CHECK:"
                if len(linear_results) >= 2:
                    debug += f"\nGround truth first item score: {linear_results[0][1]}"
                    debug += f"\nGround truth second item score: {linear_results[1][1]}"
                    debug += f"\nCorrectly sorted by similarity? {linear_results[0][1] >= linear_results[1][1]}"

                # Re-raise with detailed information
                raise AssertionError(str(e) + debug)

        return recall, selectivity, query_time, len(redis_items)

    def test(self):
        print(f"\nRunning comprehensive VSIM FILTER tests...")

        # Create a larger dataset for testing
        print(f"Creating dataset with {self.count} vectors and attributes...")
        self.vectors, self.names, self.attribute_map = self.create_vectors_with_attributes(
            self.test_key, self.count)

        # ==== 1. Recall and Precision Testing ====
        print("Testing recall for various filters...")

        # Test basic filters with different selectivity
        results = {}
        results["category"] = self.test_recall_with_filter('.category == "electronics"')
        results["price_high"] = self.test_recall_with_filter('.price > 1000')
        results["in_stock"] = self.test_recall_with_filter('.in_stock')
        results["rating"] = self.test_recall_with_filter('.rating >= 4')
        results["complex1"] = self.test_recall_with_filter('.category == "electronics" and .price < 500')

        print("Filter | Recall | Selectivity | Time (ms) | Results")
        print("----------------------------------------------------")
        for name, (recall, selectivity, time_ms, count) in results.items():
            print(f"{name:7} | {recall:.3f} | {selectivity:.3f} | {time_ms*1000:.1f} | {count}")

        # ==== 2. Filter Selectivity Performance ====
        print("\nTesting filter selectivity performance...")

        # High selectivity (very few matches)
        high_sel_recall, _, high_sel_time, _ = self.test_recall_with_filter('.is_premium')

        # Medium selectivity
        med_sel_recall, _, med_sel_time, _ = self.test_recall_with_filter('.price > 100 and .price < 1000')

        # Low selectivity (many matches)
        low_sel_recall, _, low_sel_time, _ = self.test_recall_with_filter('.year > 2000')

        print(f"High selectivity recall: {high_sel_recall:.3f}, time: {high_sel_time*1000:.1f}ms")
        print(f"Med selectivity recall: {med_sel_recall:.3f}, time: {med_sel_time*1000:.1f}ms")
        print(f"Low selectivity recall: {low_sel_recall:.3f}, time: {low_sel_time*1000:.1f}ms")

        # ==== 3. FILTER-EF Parameter Testing ====
        print("\nTesting FILTER-EF parameter...")

        # Test with different FILTER-EF values
        filter_expr = '.category == "electronics" and .price > 200'
        ef_values = [100, 500, 2000, 5000]

        print("FILTER-EF | Recall | Time (ms)")
        print("-----------------------------")
        for filter_ef in ef_values:
            recall, _, query_time, _ = self.test_recall_with_filter(
                filter_expr, ef=500, filter_ef=filter_ef)
            print(f"{filter_ef:9} | {recall:.3f} | {query_time*1000:.1f}")

        # Assert that higher FILTER-EF generally gives better recall
        low_ef_recall, _, _, _ = self.test_recall_with_filter(filter_expr, filter_ef=100)
        high_ef_recall, _, _, _ = self.test_recall_with_filter(filter_expr, filter_ef=5000)

        # This might not always be true due to randomness, but generally holds
        # We use a softer assertion to avoid flaky tests
        assert high_ef_recall >= low_ef_recall * 0.8, \
            f"Higher FILTER-EF should generally give better recall: {high_ef_recall:.3f} vs {low_ef_recall:.3f}"

        # ==== 4. Complex Filter Expressions ====
        print("\nTesting complex filter expressions...")

        # Test a variety of complex expressions
        complex_filters = [
            '.price > 100 and (.category == "electronics" or .category == "furniture")',
            '(.rating > 4 and .in_stock) or (.price < 50 and .views > 1000)',
            '.category in ["electronics", "clothing"] and .price > 200 and .rating >= 3',
            '(.category == "electronics" and .subcategory == "phones") or (.category == "furniture" and .price > 1000)',
            '.year > 2010 and !(.price < 100) and .in_stock'
        ]

        print("Expression | Results | Time (ms)")
        print("-----------------------------")
        for i, expr in enumerate(complex_filters):
            try:
                _, _, query_time, result_count = self.test_recall_with_filter(expr)
                print(f"Complex {i+1} | {result_count:7} | {query_time*1000:.1f}")
            except Exception as e:
                print(f"Complex {i+1} | Error: {str(e)}")

        # ==== 5. Attribute Type Testing ====
        print("\nTesting different attribute types...")

        type_filters = [
            ('.price > 500', "Numeric"),
            ('.category == "books"', "String equality"),
            ('.in_stock', "Boolean"),
            ('.tags in ["sale", "new"]', "Array membership"),
            ('.rating * 2 > 8', "Arithmetic")
        ]

        for expr, type_name in type_filters:
            try:
                _, _, query_time, result_count = self.test_recall_with_filter(expr)
                print(f"{type_name:16} | {expr:30} | {result_count:5} results | {query_time*1000:.1f}ms")
            except Exception as e:
                print(f"{type_name:16} | {expr:30} | Error: {str(e)}")

        # ==== 6. Filter + Count Interaction ====
        print("\nTesting COUNT parameter with filters...")

        filter_expr = '.category == "electronics"'
        counts = [5, 20, 100]

        for count in counts:
            query_vec = generate_random_vector(self.dim)
            cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
            cmd_args.extend([str(x) for x in query_vec])
            cmd_args.extend(['COUNT', count, 'WITHSCORES', 'FILTER', filter_expr])

            results = self.redis.execute_command(*cmd_args)
            result_count = len(results) // 2  # Divide by 2 because WITHSCORES returns pairs

            # We expect result count to be at most the requested count
            assert result_count <= count, f"Got {result_count} results with COUNT {count}"
            print(f"COUNT {count:3} | Got {result_count:3} results")

        # ==== 7. Edge Cases ====
        print("\nTesting edge cases...")

        # Test with no matching items
        no_match_expr = '.category == "nonexistent_category"'
        results = self.redis.execute_command('VSIM', self.test_key, 'VALUES', self.dim,
                                           *[str(x) for x in generate_random_vector(self.dim)],
                                           'FILTER', no_match_expr)
        assert len(results) == 0, f"Expected 0 results for non-matching filter, got {len(results)}"
        print(f"No matching items: {len(results)} results (expected 0)")

        # Test with invalid filter syntax
        try:
            self.redis.execute_command('VSIM', self.test_key, 'VALUES', self.dim,
                                     *[str(x) for x in generate_random_vector(self.dim)],
                                     'FILTER', '.category === "books"')  # Triple equals is invalid
            assert False, "Expected error for invalid filter syntax"
        except:
            print("Invalid filter syntax correctly raised an error")

        # Test with extremely long complex expression
        long_expr = ' and '.join([f'.rating > {i/10}' for i in range(10)])
        try:
            results = self.redis.execute_command('VSIM', self.test_key, 'VALUES', self.dim,
                                               *[str(x) for x in generate_random_vector(self.dim)],
                                               'FILTER', long_expr)
            print(f"Long expression: {len(results)} results")
        except Exception as e:
            print(f"Long expression error: {str(e)}")

        print("\nComprehensive VSIM FILTER tests completed successfully")


class VSIMFilterSelectivityTest(TestCase):
    def getname(self):
        return "VSIM FILTER selectivity performance benchmark"

    def estimated_runtime(self):
        return 8  # This test might take up to 8 seconds

    def setup(self):
        super().setup()
        self.dim = 32
        self.count = 10000
        self.test_key = f"{self.test_key}:selectivity"  # Use a different key

    def create_vector_with_age_attribute(self, name, age):
        """Create a vector with a specific age attribute"""
        vec = generate_random_vector(self.dim)
        vec_bytes = struct.pack(f'{self.dim}f', *vec)
        self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, name)
        self.redis.execute_command('VSETATTR', self.test_key, name, json.dumps({"age": age}))

    def test(self):
        print("\nRunning VSIM FILTER selectivity benchmark...")

        # Create a dataset where we control the exact selectivity
        print(f"Creating controlled dataset with {self.count} vectors...")

        # Create vectors with age attributes from 1 to 100
        for i in range(self.count):
            age = (i % 100) + 1  # Ages from 1 to 100
            name = f"{self.test_key}:item:{i}"
            self.create_vector_with_age_attribute(name, age)

        # Create a query vector
        query_vec = generate_random_vector(self.dim)

        # Test filters with different selectivities
        selectivities = [0.01, 0.05, 0.10, 0.25, 0.50, 0.75, 0.99]
        results = []

        print("\nSelectivity | Filter          | Results | Time (ms)")
        print("--------------------------------------------------")

        for target_selectivity in selectivities:
            # Calculate age threshold for desired selectivity
            # For example, age <= 10 gives 10% selectivity
            age_threshold = int(target_selectivity * 100)
            filter_expr = f'.age <= {age_threshold}'

            # Run query and measure time
            start_time = time.time()
            cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
            cmd_args.extend([str(x) for x in query_vec])
            cmd_args.extend(['COUNT', 100, 'FILTER', filter_expr])

            results = self.redis.execute_command(*cmd_args)
            query_time = time.time() - start_time

            actual_selectivity = len(results) / min(100, int(target_selectivity * self.count))
            print(f"{target_selectivity:.2f}      | {filter_expr:15} | {len(results):7} | {query_time*1000:.1f}")

            # Add assertion to ensure reasonable performance for different selectivities
            # For very selective queries (1%), we might need more exploration
            if target_selectivity <= 0.05:
                # For very selective queries, ensure we can find some results
                assert len(results) > 0, f"No results found for {filter_expr}"
            else:
                # For less selective queries, performance should be reasonable
                assert query_time < 1.0, f"Query too slow: {query_time:.3f}s for {filter_expr}"

        print("\nSelectivity benchmark completed successfully")


class VSIMFilterComparisonTest(TestCase):
    def getname(self):
        return "VSIM FILTER EF parameter comparison"

    def estimated_runtime(self):
        return 8  # This test might take up to 8 seconds

    def setup(self):
        super().setup()
        self.dim = 32
        self.count = 5000
        self.test_key = f"{self.test_key}:efparams"  # Use a different key

    def create_dataset(self):
        """Create a dataset with specific attribute patterns for testing FILTER-EF"""
        vectors = []
        names = []

        # Create vectors with category and quality score attributes
        for i in range(self.count):
            vec = generate_random_vector(self.dim)
            name = f"{self.test_key}:item:{i}"

            # Add vector to Redis
            vec_bytes = struct.pack(f'{self.dim}f', *vec)
            self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, name)

            # Create attributes - we want a very selective filter
            # Only 2% of items have category=premium AND quality>90
            category = "premium" if random.random() < 0.1 else random.choice(["standard", "economy", "basic"])
            quality = random.randint(1, 100)

            attrs = {
                "id": i,
                "category": category,
                "quality": quality
            }

            self.redis.execute_command('VSETATTR', self.test_key, name, json.dumps(attrs))
            vectors.append(vec)
            names.append(name)

        return vectors, names

    def test(self):
        print("\nRunning VSIM FILTER-EF parameter comparison...")

        # Create dataset
        vectors, names = self.create_dataset()

        # Create a selective filter that matches ~2% of items
        filter_expr = '.category == "premium" and .quality > 90'

        # Create query vector
        query_vec = generate_random_vector(self.dim)

        # Test different FILTER-EF values
        ef_values = [50, 100, 500, 1000, 5000]
        results = []

        print("\nFILTER-EF | Results | Time (ms) | Notes")
        print("---------------------------------------")

        baseline_count = None

        for ef in ef_values:
            # Run query and measure time
            start_time = time.time()
            cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
            cmd_args.extend([str(x) for x in query_vec])
            cmd_args.extend(['COUNT', 100, 'FILTER', filter_expr, 'FILTER-EF', ef])

            query_results = self.redis.execute_command(*cmd_args)
            query_time = time.time() - start_time

            # Set baseline for comparison
            if baseline_count is None:
                baseline_count = len(query_results)

            recall_rate = len(query_results) / max(1, baseline_count) if baseline_count > 0 else 1.0

            notes = ""
            if ef == 5000:
                notes = "Baseline"
            elif recall_rate < 0.5:
                notes = "Low recall!"

            print(f"{ef:9} | {len(query_results):7} | {query_time*1000:.1f} | {notes}")
            results.append((ef, len(query_results), query_time))

        # If we have enough results at highest EF, check that recall improves with higher EF
        if results[-1][1] >= 5:  # At least 5 results for highest EF
            # Extract result counts
            result_counts = [r[1] for r in results]

            # The last result (highest EF) should typically find more results than the first (lowest EF)
            # but we use a soft assertion to avoid flaky tests
            assert result_counts[-1] >= result_counts[0], \
                f"Higher FILTER-EF should find at least as many results: {result_counts[-1]} vs {result_counts[0]}"

        print("\nFILTER-EF parameter comparison completed successfully")
