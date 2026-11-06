from test import TestCase, fill_redis_with_vectors, generate_random_vector
import threading, time

class ConcurrentVSIMAndDEL(TestCase):
    def getname(self):
        return "Concurrent VSIM and DEL operations"

    def estimated_runtime(self):
        return 2

    def test(self):
        # Fill the key with 5000 random vectors
        dim = 128
        count = 5000
        fill_redis_with_vectors(self.redis, self.test_key, count, dim)

        # List to store results from threads
        thread_results = []

        def vsim_thread():
            """Thread function to perform VSIM operations until the key is deleted"""
            while True:
                query_vec = generate_random_vector(dim)
                result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', dim,
                                                   *[str(x) for x in query_vec], 'COUNT', 10)
                if not result:
                    # Empty array detected, key is deleted
                    thread_results.append(True)
                    break

        # Start multiple threads to perform VSIM operations
        threads = []
        for _ in range(4):  # Start 4 threads
            t = threading.Thread(target=vsim_thread)
            t.start()
            threads.append(t)

        # Delete the key while threads are still running
        time.sleep(1)
        self.redis.delete(self.test_key)

        # Wait for all threads to finish (they will exit once they detect the key is deleted)
        for t in threads:
            t.join()

        # Verify that all threads detected an empty array or error
        assert len(thread_results) == len(threads), "Not all threads detected the key deletion"
        assert all(thread_results), "Some threads did not detect an empty array or error after DEL"
