from test import TestCase, generate_random_vector
import threading
import time
import struct

class ThreadingStressTest(TestCase):
    def getname(self):
        return "Concurrent VADD/DEL/VSIM operations stress test"

    def estimated_runtime(self):
        return 10  # Test runs for 10 seconds

    def test(self):
        # Constants - easy to modify if needed
        NUM_VADD_THREADS = 10
        NUM_VSIM_THREADS = 1
        NUM_DEL_THREADS = 1
        TEST_DURATION = 10  # seconds
        VECTOR_DIM = 100
        DEL_INTERVAL = 1  # seconds

        # Shared flags and state
        stop_event = threading.Event()
        error_list = []
        error_lock = threading.Lock()

        def log_error(thread_name, error):
            with error_lock:
                error_list.append(f"{thread_name}: {error}")

        def vadd_worker(thread_id):
            """Thread function to perform VADD operations"""
            thread_name = f"VADD-{thread_id}"
            try:
                vector_count = 0
                while not stop_event.is_set():
                    try:
                        # Generate random vector
                        vec = generate_random_vector(VECTOR_DIM)
                        vec_bytes = struct.pack(f'{VECTOR_DIM}f', *vec)

                        # Add vector with CAS option
                        self.redis.execute_command(
                            'VADD',
                            self.test_key,
                            'FP32',
                            vec_bytes,
                            f'{self.test_key}:item:{thread_id}:{vector_count}',
                            'CAS'
                        )

                        vector_count += 1

                        # Small sleep to reduce CPU pressure
                        if vector_count % 10 == 0:
                            time.sleep(0.001)
                    except Exception as e:
                        log_error(thread_name, f"Error: {str(e)}")
                        time.sleep(0.1)  # Slight backoff on error
            except Exception as e:
                log_error(thread_name, f"Thread error: {str(e)}")

        def del_worker():
            """Thread function that deletes the key periodically"""
            thread_name = "DEL"
            try:
                del_count = 0
                while not stop_event.is_set():
                    try:
                        # Sleep first, then delete
                        time.sleep(DEL_INTERVAL)
                        if stop_event.is_set():
                            break

                        self.redis.delete(self.test_key)
                        del_count += 1
                    except Exception as e:
                        log_error(thread_name, f"Error: {str(e)}")
            except Exception as e:
                log_error(thread_name, f"Thread error: {str(e)}")

        def vsim_worker(thread_id):
            """Thread function to perform VSIM operations"""
            thread_name = f"VSIM-{thread_id}"
            try:
                search_count = 0
                while not stop_event.is_set():
                    try:
                        # Generate query vector
                        query_vec = generate_random_vector(VECTOR_DIM)
                        query_str = [str(x) for x in query_vec]

                        # Perform similarity search
                        args = ['VSIM', self.test_key, 'VALUES', VECTOR_DIM]
                        args.extend(query_str)
                        args.extend(['COUNT', 10])
                        self.redis.execute_command(*args)

                        search_count += 1

                        # Small sleep to reduce CPU pressure
                        if search_count % 10 == 0:
                            time.sleep(0.005)
                    except Exception as e:
                        # Don't log empty array errors, as they're expected when key doesn't exist
                        if "empty array" not in str(e).lower():
                            log_error(thread_name, f"Error: {str(e)}")
                        time.sleep(0.1)  # Slight backoff on error
            except Exception as e:
                log_error(thread_name, f"Thread error: {str(e)}")

        # Start all threads
        threads = []

        # VADD threads
        for i in range(NUM_VADD_THREADS):
            thread = threading.Thread(target=vadd_worker, args=(i,))
            thread.start()
            threads.append(thread)

        # DEL threads
        for _ in range(NUM_DEL_THREADS):
            thread = threading.Thread(target=del_worker)
            thread.start()
            threads.append(thread)

        # VSIM threads
        for i in range(NUM_VSIM_THREADS):
            thread = threading.Thread(target=vsim_worker, args=(i,))
            thread.start()
            threads.append(thread)

        # Let the test run for the specified duration
        time.sleep(TEST_DURATION)

        # Signal all threads to stop
        stop_event.set()

        # Wait for threads to finish
        for thread in threads:
            thread.join(timeout=2.0)

        # Check if Redis is still responsive
        try:
            ping_result = self.redis.ping()
            assert ping_result, "Redis did not respond to PING after stress test"
        except Exception as e:
            assert False, f"Redis connection failed after stress test: {str(e)}"

        # Report any errors for diagnosis, but don't fail the test unless PING fails
        if error_list:
            error_count = len(error_list)
            print(f"\nEncountered {error_count} errors during stress test.")
            print("First 5 errors:")
            for error in error_list[:5]:
                print(f"- {error}")
