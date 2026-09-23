from test import TestCase, fill_redis_with_vectors, generate_random_vector
import redis
import threading


class ConcurrentVSIMAndVREM(TestCase):
    def getname(self):
        return "Concurrent VSIM and VREM operations"

    def estimated_runtime(self):
        return 5

    def test(self):
        # Concurrent VSIM (background search threads) and VREM (element removal)
        # must not corrupt the HNSW graph: VREM frees nodes that a running VSIM
        # may still be reading. Keep VSIM searches running on their own
        # connections while the main thread churns elements in and out of a
        # populated set, then verify the server is still alive.
        dim = 128
        base_count = 2000
        iterations = 5000
        num_vsim_threads = 4

        # Fill the key with base vectors so the set never empties during churn
        fill_redis_with_vectors(self.redis, self.test_key, base_count, dim)

        # VSIM only races VREM when it runs in a background thread
        cfg = 'vset-force-single-threaded-execution'
        original_cfg = self.redis.config_get(cfg).get(cfg, 'no')
        self.redis.config_set(cfg, 'no')

        stop = threading.Event()

        def vsim_worker():
            """Thread function to perform VSIM operations until stopped"""
            conn = redis.Redis(port=self.primary_port, db=9)
            while not stop.is_set():
                query = [str(x) for x in generate_random_vector(dim)]
                try:
                    conn.execute_command('VSIM', self.test_key, 'VALUES', dim,
                                         *query, 'COUNT', 10)
                except redis.exceptions.RedisError:
                    pass

        # Start VSIM threads, each on its own connection
        threads = [threading.Thread(target=vsim_worker)
                   for _ in range(num_vsim_threads)]
        for t in threads:
            t.start()

        # Repeatedly add and remove an element while the searches run
        try:
            churn_vec = [str(x) for x in generate_random_vector(dim)]
            for i in range(iterations):
                elem = f'churn:{i % 64}'
                self.redis.execute_command('VADD', self.test_key, 'VALUES', dim,
                                           *churn_vec, elem)
                self.redis.execute_command('VREM', self.test_key, elem)
        finally:
            stop.set()
            for t in threads:
                t.join(timeout=5.0)
            try:
                self.redis.config_set(cfg, original_cfg)
            except redis.exceptions.RedisError:
                pass  # server may be down if the race crashed it

        assert self.redis.ping(), "Redis crashed during concurrent VSIM/VREM"
