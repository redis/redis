from test import TestCase, generate_random_vector
import struct
import redis.exceptions


class VaddQuantOptionConflict(TestCase):
    def getname(self):
        return "[regression] VADD quant option oneof + replication compatibility"

    def estimated_runtime(self):
        return 0.1

    def _attempt(self, opts, as_fp32=True):
        key = self.test_key
        self.redis.delete(key)
        dim = 2
        vec = generate_random_vector(dim)
        # FP32: 4 bytes per component. VALUES expects `dim` separate numbers;
        # a packed blob is only valid with the FP32 format specifier.
        if as_fp32:
            blob = struct.pack(f'{dim}f', *vec)
            vector_args = ['FP32', blob]
        else:
            vector_args = ['VALUES', dim] + [str(v) for v in vec]
        args = ['VADD', key] + vector_args + [key + ':item'] + opts
        try:
            return self.redis.execute_command(*args)
        except redis.exceptions.ResponseError as e:
            return e

    def test(self):
        # NOQUANT, BIN, Q8 form a oneof group (commands.json). Client commands
        # supplying two DIFFERENT options must error, in any order, and the
        # rejected command must leave the key absent.
        for opts in (['NOQUANT', 'BIN'], ['BIN', 'NOQUANT'],
                     ['Q8', 'BIN'], ['NOQUANT', 'Q8'],
                     ['BIN', 'Q8', 'NOQUANT']):
            result = self._attempt(opts)
            assert isinstance(result, redis.exceptions.ResponseError), (
                f"VADD conflicting options {opts} should error, got {result!r}")
            assert 'quantization' in str(result).lower(), (
                f"Unexpected error for {opts}: {result}")
            assert self.redis.exists(self.test_key) == 0, (
                f"key absent expected after rejected VADD {opts}")

        # Repeating the SAME flag is a no-op and stays accepted: clients that
        # always append a default flag must not break (compatibility fix).
        for opts in (['NOQUANT', 'NOQUANT'], ['BIN', 'BIN'], ['Q8', 'Q8']):
            result = self._attempt(opts)
            assert result == 1, (
                f"VADD repeated identical option {opts} should succeed, got {result!r}")
            assert self.redis.exists(self.test_key) == 1

        # A single quantization option works with both vector encodings.
        for opts in (['NOQUANT'], ['BIN'], ['Q8']):
            result = self._attempt(opts)
            assert result == 1, f"VADD single option {opts} should succeed, got {result!r}"
            assert self.redis.exists(self.test_key) == 1
            result = self._attempt(opts, as_fp32=False)
            assert result == 1, f"VADD single option {opts} + VALUES should succeed"

        # Non-quantization options are unaffected.
        result = self._attempt(['CAS', 'BIN'])
        assert result == 1, f"VADD CAS+BIN should succeed, got {result!r}"

        self.redis.delete(self.test_key)
