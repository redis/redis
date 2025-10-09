#!/usr/bin/env python3
"""
Redis Table Module - Advanced Memory Profiler
Detects memory leaks using statistical analysis and trend detection
"""

import redis
import time
import sys
import subprocess
import os
from typing import List, Tuple

class MemoryProfiler:
    def __init__(self, auto_start=True):
        self.redis_process = None
        self.auto_started = False
        
        # Try to connect first
        self.r = redis.Redis(host='localhost', port=6379, decode_responses=True)
        try:
            self.r.ping()
            print("✓ Connected to existing Redis instance")
        except:
            if auto_start:
                print("Redis not running, starting Redis server...")
                self._start_redis()
            else:
                print("ERROR: Cannot connect to Redis")
                print("Start Redis with: redis-server --loadmodule redis_table.so")
                sys.exit(1)
    
    def _start_redis(self):
        """Start Redis server with the table module"""
        script_dir = os.path.dirname(os.path.abspath(__file__))
        module_path = os.path.join(script_dir, '..', 'redis_table.so')
        redis_dir = os.path.join(script_dir, '..', '..', '..')
        redis_server = os.path.join(redis_dir, 'src', 'redis-server')
        
        if not os.path.exists(module_path):
            print(f"ERROR: Module not found at {module_path}")
            print("Run 'make' first to build the module")
            sys.exit(1)
        
        if not os.path.exists(redis_server):
            print(f"ERROR: Redis server not found at {redis_server}")
            sys.exit(1)
        
        # Kill any existing Redis
        subprocess.run(['pkill', '-9', 'redis-server'], 
                      stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
        time.sleep(1)
        
        # Start Redis
        cmd = [
            redis_server,
            '--loadmodule', module_path,
            '--daemonize', 'yes',
            '--port', '6379',
            '--save', '',
            '--appendonly', 'no'
        ]
        
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"ERROR: Failed to start Redis: {result.stderr}")
            sys.exit(1)
        
        time.sleep(2)
        
        # Verify connection
        try:
            self.r.ping()
            print("✓ Redis server started successfully")
            self.auto_started = True
        except:
            print("ERROR: Redis started but cannot connect")
            sys.exit(1)
    
    def _stop_redis(self):
        """Stop Redis if we started it"""
        if self.auto_started:
            print("\nStopping Redis server...")
            try:
                self.r.shutdown(nosave=True)
            except:
                pass
            time.sleep(1)
            print("✓ Redis server stopped")
    
    def get_memory_stats(self) -> dict:
        """Get detailed memory statistics"""
        info = self.r.info('memory')
        return {
            'used_memory': int(info['used_memory']),
            'used_memory_rss': int(info['used_memory_rss']),
            'used_memory_peak': int(info['used_memory_peak']),
            'mem_fragmentation_ratio': float(info['mem_fragmentation_ratio']),
            'allocator_allocated': int(info.get('allocator_allocated', 0)),
            'allocator_active': int(info.get('allocator_active', 0)),
            'allocator_resident': int(info.get('allocator_resident', 0))
        }
    
    def cleanup(self):
        """Clean up all test data"""
        self.r.flushall()
    
    def detect_leak(self, samples: List[int], threshold: float = 0.1) -> Tuple[bool, str]:
        """
        Detect memory leak using linear regression
        Returns (is_leak, description)
        """
        if len(samples) < 3:
            return False, "Not enough samples"
        
        # Calculate trend
        n = len(samples)
        x_mean = sum(range(n)) / n
        y_mean = sum(samples) / n
        
        numerator = sum((i - x_mean) * (samples[i] - y_mean) for i in range(n))
        denominator = sum((i - x_mean) ** 2 for i in range(n))
        
        if denominator == 0:
            return False, "No variance in data"
        
        slope = numerator / denominator
        
        # Calculate percentage increase
        if samples[0] == 0:
            return False, "Initial memory is zero"
        
        percent_increase = (slope * n) / samples[0] * 100
        
        if percent_increase > threshold:
            return True, f"Memory increasing at {percent_increase:.2f}% per operation"
        else:
            return False, f"Memory stable (trend: {percent_increase:.2f}%)"
    
    def test_schema_creation_leak(self, iterations: int = 100) -> dict:
        """Test for leaks in schema creation"""
        print(f"\n{'='*60}")
        print(f"Testing Schema Creation ({iterations} iterations)")
        print(f"{'='*60}")
        
        self.cleanup()
        samples = []
        
        # Baseline
        baseline = self.get_memory_stats()['used_memory']
        samples.append(baseline)
        
        # Create schemas
        for i in range(iterations):
            self.r.execute_command('TABLE.NAMESPACE.CREATE', f'ns_{i}')
            self.r.execute_command('TABLE.SCHEMA.CREATE', f'ns_{i}.table',
                                  'ID:integer:true',
                                  'NAME:string:false',
                                  'VALUE:integer:false')
            
            if i % 10 == 0:
                mem = self.get_memory_stats()['used_memory']
                samples.append(mem)
        
        final = self.get_memory_stats()['used_memory']
        samples.append(final)
        
        is_leak, description = self.detect_leak(samples)
        
        result = {
            'test': 'schema_creation',
            'iterations': iterations,
            'baseline': baseline,
            'final': final,
            'increase': final - baseline,
            'per_operation': (final - baseline) / iterations,
            'is_leak': is_leak,
            'description': description,
            'samples': samples
        }
        
        print(f"Baseline:       {baseline:,} bytes")
        print(f"Final:          {final:,} bytes")
        print(f"Increase:       {final - baseline:,} bytes")
        print(f"Per operation:  {result['per_operation']:.2f} bytes")
        print(f"Status:         {'⚠ LEAK DETECTED' if is_leak else '✓ OK'}")
        print(f"Analysis:       {description}")
        
        self.cleanup()
        return result
    
    def test_schema_alteration_leak(self, iterations: int = 100) -> dict:
        """Test for leaks in schema alterations"""
        print(f"\n{'='*60}")
        print(f"Testing Schema Alteration ({iterations} iterations)")
        print(f"{'='*60}")
        
        self.cleanup()
        
        # Setup
        self.r.execute_command('TABLE.NAMESPACE.CREATE', 'test')
        self.r.execute_command('TABLE.SCHEMA.CREATE', 'test.data',
                              'ID:integer:true')
        
        samples = []
        baseline = self.get_memory_stats()['used_memory']
        samples.append(baseline)
        
        # Add and drop columns repeatedly
        for i in range(iterations):
            self.r.execute_command('TABLE.SCHEMA.ALTER', 'test.data',
                                  'ADD', 'COLUMN', f'col_{i}:string:false')
            
            if i % 10 == 0:
                mem = self.get_memory_stats()['used_memory']
                samples.append(mem)
        
        final = self.get_memory_stats()['used_memory']
        samples.append(final)
        
        is_leak, description = self.detect_leak(samples)
        
        result = {
            'test': 'schema_alteration',
            'iterations': iterations,
            'baseline': baseline,
            'final': final,
            'increase': final - baseline,
            'per_operation': (final - baseline) / iterations,
            'is_leak': is_leak,
            'description': description
        }
        
        print(f"Baseline:       {baseline:,} bytes")
        print(f"Final:          {final:,} bytes")
        print(f"Increase:       {final - baseline:,} bytes")
        print(f"Per operation:  {result['per_operation']:.2f} bytes")
        print(f"Status:         {'⚠ LEAK DETECTED' if is_leak else '✓ OK'}")
        print(f"Analysis:       {description}")
        
        self.cleanup()
        return result
    
    def test_index_cycle_leak(self, iterations: int = 100) -> dict:
        """Test for leaks in index add/drop cycles"""
        print(f"\n{'='*60}")
        print(f"Testing Index Add/Drop Cycles ({iterations} iterations)")
        print(f"{'='*60}")
        
        self.cleanup()
        
        # Setup with data
        self.r.execute_command('TABLE.NAMESPACE.CREATE', 'test')
        self.r.execute_command('TABLE.SCHEMA.CREATE', 'test.data',
                              'ID:integer:false',
                              'VALUE:integer:false')
        
        # Insert data
        for i in range(100):
            self.r.execute_command('TABLE.INSERT', 'test.data',
                                  f'ID={i}', f'VALUE={i*10}')
        
        samples = []
        baseline = self.get_memory_stats()['used_memory']
        samples.append(baseline)
        
        # Add and drop index repeatedly
        for i in range(iterations):
            self.r.execute_command('TABLE.SCHEMA.ALTER', 'test.data',
                                  'ADD', 'INDEX', 'VALUE')
            self.r.execute_command('TABLE.SCHEMA.ALTER', 'test.data',
                                  'DROP', 'INDEX', 'VALUE')
            
            if i % 10 == 0:
                mem = self.get_memory_stats()['used_memory']
                samples.append(mem)
        
        final = self.get_memory_stats()['used_memory']
        samples.append(final)
        
        is_leak, description = self.detect_leak(samples, threshold=0.05)
        
        result = {
            'test': 'index_cycles',
            'iterations': iterations,
            'baseline': baseline,
            'final': final,
            'increase': final - baseline,
            'per_operation': (final - baseline) / iterations,
            'is_leak': is_leak,
            'description': description
        }
        
        print(f"Baseline:       {baseline:,} bytes")
        print(f"Final:          {final:,} bytes")
        print(f"Increase:       {final - baseline:,} bytes")
        print(f"Per operation:  {result['per_operation']:.2f} bytes")
        print(f"Status:         {'⚠ LEAK DETECTED' if is_leak else '✓ OK'}")
        print(f"Analysis:       {description}")
        
        self.cleanup()
        return result
    
    def test_query_operation_leak(self, iterations: int = 1000) -> dict:
        """Test for leaks in query operations"""
        print(f"\n{'='*60}")
        print(f"Testing Query Operations ({iterations} iterations)")
        print(f"{'='*60}")
        
        self.cleanup()
        
        # Setup
        self.r.execute_command('TABLE.NAMESPACE.CREATE', 'test')
        self.r.execute_command('TABLE.SCHEMA.CREATE', 'test.data',
                              'ID:integer:true',
                              'VALUE:integer:false')
        
        # Insert data
        for i in range(100):
            self.r.execute_command('TABLE.INSERT', 'test.data',
                                  f'ID={i}', f'VALUE={i*10}')
        
        samples = []
        baseline = self.get_memory_stats()['used_memory']
        samples.append(baseline)
        
        # Run queries
        for i in range(iterations):
            self.r.execute_command('TABLE.SELECT', 'test.data',
                                  'WHERE', f'ID={i % 100}')
            
            if i % 100 == 0:
                mem = self.get_memory_stats()['used_memory']
                samples.append(mem)
        
        final = self.get_memory_stats()['used_memory']
        samples.append(final)
        
        is_leak, description = self.detect_leak(samples, threshold=0.01)
        
        result = {
            'test': 'query_operations',
            'iterations': iterations,
            'baseline': baseline,
            'final': final,
            'increase': final - baseline,
            'per_operation': (final - baseline) / iterations,
            'is_leak': is_leak,
            'description': description
        }
        
        print(f"Baseline:       {baseline:,} bytes")
        print(f"Final:          {final:,} bytes")
        print(f"Increase:       {final - baseline:,} bytes")
        print(f"Per operation:  {result['per_operation']:.4f} bytes")
        print(f"Status:         {'⚠ LEAK DETECTED' if is_leak else '✓ OK'}")
        print(f"Analysis:       {description}")
        
        self.cleanup()
        return result
    
    def test_crud_cycle_leak(self, iterations: int = 500) -> dict:
        """Test for leaks in CRUD operation cycles"""
        print(f"\n{'='*60}")
        print(f"Testing CRUD Cycles ({iterations} iterations)")
        print(f"{'='*60}")
        
        self.cleanup()
        
        # Setup
        self.r.execute_command('TABLE.NAMESPACE.CREATE', 'test')
        self.r.execute_command('TABLE.SCHEMA.CREATE', 'test.data',
                              'ID:integer:true',
                              'VALUE:integer:false')
        
        samples = []
        baseline = self.get_memory_stats()['used_memory']
        samples.append(baseline)
        
        # CRUD cycles
        for i in range(iterations):
            # Create
            row_id = self.r.execute_command('TABLE.INSERT', 'test.data',
                                           f'ID={i}', f'VALUE={i}')
            # Read
            self.r.execute_command('TABLE.SELECT', 'test.data',
                                  'WHERE', f'ID={i}')
            # Update
            self.r.execute_command('TABLE.UPDATE', 'test.data',
                                  'WHERE', f'ID={i}',
                                  'SET', f'VALUE={i*2}')
            # Delete
            self.r.execute_command('TABLE.DELETE', 'test.data',
                                  'WHERE', f'ID={i}')
            
            if i % 50 == 0:
                mem = self.get_memory_stats()['used_memory']
                samples.append(mem)
        
        final = self.get_memory_stats()['used_memory']
        samples.append(final)
        
        is_leak, description = self.detect_leak(samples, threshold=0.05)
        
        result = {
            'test': 'crud_cycles',
            'iterations': iterations,
            'baseline': baseline,
            'final': final,
            'increase': final - baseline,
            'per_operation': (final - baseline) / iterations,
            'is_leak': is_leak,
            'description': description
        }
        
        print(f"Baseline:       {baseline:,} bytes")
        print(f"Final:          {final:,} bytes")
        print(f"Increase:       {final - baseline:,} bytes")
        print(f"Per operation:  {result['per_operation']:.4f} bytes")
        print(f"Status:         {'⚠ LEAK DETECTED' if is_leak else '✓ OK'}")
        print(f"Analysis:       {description}")
        
        self.cleanup()
        return result

def main():
    print("="*60)
    print("Redis Table Module - Memory Profiler")
    print("="*60)
    print()
    
    profiler = MemoryProfiler(auto_start=True)
    results = []
    
    try:
        # Run all tests
        results.append(profiler.test_schema_creation_leak(100))
        results.append(profiler.test_schema_alteration_leak(100))
        results.append(profiler.test_index_cycle_leak(100))
        results.append(profiler.test_query_operation_leak(1000))
        results.append(profiler.test_crud_cycle_leak(500))
        
        # Summary
        print(f"\n{'='*60}")
        print("Memory Profiler Summary")
        print(f"{'='*60}")
        
        leaks_detected = 0
        for result in results:
            status = "⚠ LEAK" if result['is_leak'] else "✓ OK"
            print(f"{result['test']:25s} {status:10s} {result['per_operation']:10.2f} bytes/op")
            if result['is_leak']:
                leaks_detected += 1
        
        print(f"{'='*60}")
        print(f"Total tests:    {len(results)}")
        print(f"Leaks detected: {leaks_detected}")
        print(f"Status:         {'⚠ LEAKS FOUND' if leaks_detected > 0 else '✓ ALL CLEAR'}")
        print(f"{'='*60}")
        
        return 0 if leaks_detected == 0 else 1
    
    finally:
        # Always stop Redis if we started it
        profiler._stop_redis()

if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"\nERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
