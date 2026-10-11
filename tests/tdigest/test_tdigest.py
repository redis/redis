
from ast import arg
from support import *
import math
import random
from random import randint


def parse_tdigest_info(array_reply):
    reply_dict = {}
    for pos in range(0, len(array_reply), 2):
        property_name = array_reply[pos]
        property_value = array_reply[pos + 1]
        reply_dict[property_name] = property_value
    return reply_dict


class testTDigest:
    def __init__(self):
        self.env = Env(decodeResponses=True)
        self.assertOk = self.env.assertTrue
        self.cmd = self.env.cmd
        self.assertEqual = self.env.assertEqual
        self.assertRaises = self.env.assertRaises
        self.assertTrue = self.env.assertTrue
        self.assertAlmostEqual = self.env.assertAlmostEqual
        self.assertGreater = self.env.assertGreater
        self.assertAlmostEqual = self.env.assertAlmostEqual
        self.restart_and_reload = self.env.restartAndReload

    def test_tdigest_create(self):
        self.cmd('FLUSHALL')
        for compression in range(100, 1000, 100):
            self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", compression))
            self.assertEqual(
                compression,
                parse_tdigest_info(self.cmd("tdigest.info", "tdigest"))["Compression"],
            )
            self.assertOk(self.cmd("del", "tdigest"))
        self.assertOk(self.cmd("tdigest.create", "tdigest-default-compression"))
        self.assertEqual(
                100,
                parse_tdigest_info(self.cmd("tdigest.info", "tdigest-default-compression"))["Compression"],
            )

    def test_tdigest_create_twice(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest"))
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.create", "tdigest")

    def test_negative_tdigest_create(self):
        self.cmd('FLUSHALL')
        self.cmd("SET", "tdigest", "B")
        # WRONGTYPE
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.create", "tdigest", "compression", 100
        )
        self.cmd("DEL", "tdigest")

        # arity upper
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.create", "tdigest", 100, 5,
        )
        # missing
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.create", "tdigest", "compression"
        )
        # parsing
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.create", "tdigest", "compression", "a"
        )
        # compression negative/zero value
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.create", "tdigest", "compression", 0
        )
        # compression negative/zero value
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.create", "tdigest", "compression", -1
        )
        # wrong keyword
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.create", "tdigest", "string", 100
        )
        self.cmd('FLUSHALL')
        # failed allocation
        self.assertRaises(
           ResponseError, self.cmd, "tdigest.create", "tdigest", "compression", '100000000000000000000'
        )

    def test_tdigest_reset(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", 100))
        # reset on empty histogram
        self.assertOk(self.cmd("tdigest.reset", "tdigest"))
        # insert datapoints into sketch
        for x in range(100):
            self.assertOk(self.cmd("tdigest.add", "tdigest", random.random()))

        # assert we have 100 unmerged nodes
        self.assertEqual(
            100,
            parse_tdigest_info(self.cmd("tdigest.info", "tdigest"))["Unmerged nodes"],
        )

        self.assertOk(self.cmd("tdigest.reset", "tdigest"))

        # assert we have 100 unmerged nodes
        self.assertEqual(
            0, parse_tdigest_info(self.cmd("tdigest.info", "tdigest"))["Unmerged nodes"]
        )

    def test_negative_tdigest_reset(self):
        self.cmd('FLUSHALL')
        self.cmd("SET", "tdigest", "B")
        # WRONGTYPE
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.reset", "tdigest"
        )
        self.cmd("DEL", "tdigest")
        # empty key
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.reset", "tdigest"
        )

        self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", 100))
        # arity lower
        self.assertRaises(ResponseError, self.cmd, "tdigest.reset")
        # arity upper
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.reset", "tdigest", 100
        )

    def test_tdigest_add(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", 100))
        # reset on empty histogram
        self.assertOk(self.cmd("tdigest.reset", "tdigest"))
        # insert datapoints into sketch
        for x in range(10000):
            self.assertOk(
                self.cmd(
                    "tdigest.add",
                    "tdigest",
                    random.random() * 10000,
                )
            )

        # check that multiple datapoints insertion behaves as expected
        self.assertOk(self.cmd("tdigest.create", "tdigest2"))
        args = ["tdigest.add", "tdigest2"]
        for x in range(100):
            args.append("1.0")
        self.assertOk(
                self.cmd(
                    " ".join(args)
                )
            )
        td_info = parse_tdigest_info(self.cmd("tdigest.info", "tdigest2"))
        total_weight = float(td_info["Merged weight"]) + float(
            td_info["Unmerged weight"]
        )
        self.assertEqual(100.0, total_weight)
        # total weight
        self.assertEqual(
                100.0,
                int(td_info["Observations"]),
            )

    def test_negative_tdigest_add(self):
        self.cmd('FLUSHALL')
        self.cmd("SET", "tdigest", "B")
        # WRONGTYPE
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.add", "tdigest", 100
        )
        self.cmd("DEL", "tdigest")
        self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", 100))
        # arity lower
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.add", "tdigest"
        )
        # key does not exist
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.add", "dont-exist", 100
        )
        # parsing
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.add", "tdigest", "a", 5, 10, 20
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.add", "tdigest", 5.0, "a", 10, 20
        )
        # ensure nothing was added given  at least one input is not a valid floating-point value
        td_info = parse_tdigest_info(self.cmd("tdigest.info", "tdigest"))
        total_weight = float(td_info["Merged weight"]) + float(
            td_info["Unmerged weight"]
        )
        self.assertEqual(0, total_weight)

        # val parameter needs to be a finite number
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.add", "tdigest", "-inf",
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.add", "tdigest", "+inf",
        )

    def test_tdigest_merge_to_empty(self):
        self.cmd("FLUSHALL")
        self.assertOk(self.cmd("tdigest.create", "to-tdigest{1}", "compression", 100))
        self.assertOk(self.cmd("tdigest.create", "from-tdigest{1}", "compression", 100))
        # insert datapoints into sketch
        for _ in range(100):
            self.assertOk(self.cmd("tdigest.add", "from-tdigest{1}", 1.0))
        # merge from-tdigest into to-tdigest
        self.assertOk(self.cmd("tdigest.merge", "to-tdigest{1}", 1 ,"from-tdigest{1}"))
        # assert we have same merged weight on both histograms ( given the to-histogram was empty )
        from_info = parse_tdigest_info(self.cmd("tdigest.info", "from-tdigest{1}"))
        total_weight_from = float(from_info["Merged weight"]) + float(
            from_info["Unmerged weight"]
        )
        to_info = parse_tdigest_info(self.cmd("tdigest.info", "to-tdigest{1}"))
        total_weight_to = float(to_info["Merged weight"]) + float(
            to_info["Unmerged weight"]
        )
        self.assertEqual(total_weight_from, total_weight_to)

    def test_tdigest_merge_itself(self):
        self.cmd("FLUSHALL")
        self.assertOk(self.cmd("tdigest.create", "to-tdigest", "compression", 100))
        # insert datapoints into sketch
        for _ in range(100):
            self.assertOk(self.cmd("tdigest.add", "to-tdigest", 1.0))
        # we should now have 100 weight on to-histogram
        to_info = parse_tdigest_info(self.cmd("tdigest.info", "to-tdigest"))
        total_weight_to = float(to_info["Merged weight"]) + float(
            to_info["Unmerged weight"]
        )
        self.assertEqual(100, total_weight_to)
        previous_weight = total_weight_to
        for iteration in range(5):
            self.assertOk(self.cmd("tdigest.merge", "to-tdigest", 1, "to-tdigest"))
            # we should now have ( iteration + 1 ) * 100 weight on to-histogram
            to_info = parse_tdigest_info(self.cmd("tdigest.info", "to-tdigest"))
            total_weight_to = float(to_info["Merged weight"]) + float(
                to_info["Unmerged weight"]
            )
            self.assertEqual(previous_weight * 2, total_weight_to)
            previous_weight = total_weight_to

    def test_tdigest_merge(self):
        self.cmd("FLUSHALL")
        to_tdigest = "to-tdigest{1}"
        from_tdigest = "from-tdigest{1}"
        self.assertOk(self.cmd("tdigest.create", to_tdigest, "compression", 100))
        self.assertOk(self.cmd("tdigest.create", from_tdigest, "compression", 100))
        # insert datapoints into sketch
        for _ in range(100):
            self.assertOk(self.cmd("tdigest.add", from_tdigest, 1.0))
        for _ in range(1000):
            self.assertOk(self.cmd("tdigest.add", to_tdigest, 1.0))
        # merge from-tdigest into to-tdigest
        self.assertOk(self.cmd("tdigest.merge", to_tdigest, 1, from_tdigest))
        # we should now have 1100 weight on to-histogram
        to_info = parse_tdigest_info(self.cmd("tdigest.info", to_tdigest))
        total_weight_to = float(to_info["Merged weight"]) + float(
            to_info["Unmerged weight"]
        )
        self.assertEqual(1100, total_weight_to)
        self.cmd("FLUSHALL")
        self.assertOk(self.cmd("tdigest.create", "to-1{1}", "compression", 55))
        self.assertOk(self.cmd("tdigest.create", "from-1{1}", "compression", 100))
        self.assertOk(self.cmd("tdigest.create", "from-2{1}", "compression", 200))
        self.assertOk(self.cmd("tdigest.create", "from-3{1}", "compression", 300))
        # insert datapoints into sketch
        self.assertOk(self.cmd("tdigest.add", "from-1{1}", 1.0))
        for _ in range(0,10):
            self.assertOk(self.cmd("tdigest.add", "from-2{1}", 1.0))
        # merge to a t-digest with max compression of all inputs which is 200
        self.assertOk(self.cmd("tdigest.merge", "to-tdigest-100{1}", "2", "from-1{1}", "from-2{1}"))
        to_info = parse_tdigest_info(self.cmd("tdigest.info", "to-tdigest-100{1}"))
        # ensure that the destination t-digest has the largest compression of all input t-digests
        compression = int(to_info["Compression"])
        self.assertEqual(200, compression)
        # assert we have same merged weight on both histograms ( given the to-histogram was empty )
        total_weight_to = float(to_info["Merged weight"]) + float(
            to_info["Unmerged weight"]
        )
        total_weight_from = 10.0 + 1.0
        self.assertEqual(total_weight_from, total_weight_to)

        # merge to a t-digest that already exists so we will preserve its compression
        self.assertOk(self.cmd("tdigest.merge", "to-1{1}", "2", "from-1{1}", "from-2{1}"))
        to_info = parse_tdigest_info(self.cmd("tdigest.info", "to-1{1}"))
        # ensure that the destination t-digest has the largest compression of all input t-digests
        compression = int(to_info["Compression"])
        self.assertEqual(55, compression)

        # merge to a t-digest with non-default compression
        self.assertOk(self.cmd("tdigest.merge", "to-tdigest-50{1}", "2","from-1{1}", "from-2{1}", "COMPRESSION", "50"))
        # ensure that the destination t-digest has the passed compression
        to_info = parse_tdigest_info(self.cmd("tdigest.info", "to-tdigest-50{1}"))
        compression = int(to_info["Compression"])
        self.assertEqual(50, compression)

        # merge to a t-digest that already exists with non-default compression
        self.assertOk(self.cmd("tdigest.merge", "to-tdigest-50{1}", "2","from-1{1}", "from-2{1}", "COMPRESSION", "500"))
        # ensure that the destination t-digest has the passed compression
        to_info = parse_tdigest_info(self.cmd("tdigest.info", "to-tdigest-50{1}"))
        compression = int(to_info["Compression"])
        self.assertEqual(500, compression)

        # merge to a t-digest that already exists but given we specify override it will use the max compression of all inputs
        self.assertOk(self.cmd("tdigest.merge", "to-tdigest-50{1}", "2","from-1{1}", "from-2{1}", "OVERRIDE"))
        # ensure that the destination t-digest has the passed compression
        to_info = parse_tdigest_info(self.cmd("tdigest.info", "to-tdigest-50{1}"))
        compression = int(to_info["Compression"])
        self.assertEqual(200, compression)

    def test_tdigest_merge_percentile(self):
        self.cmd("FLUSHALL")
        self.assertOk(self.cmd("tdigest.create", "from-1{1}", "compression", 500))
        # insert datapoints into sketch
        for x in range(1, 10000):
            self.assertOk(self.cmd("tdigest.add", "from-1{1}", x * 0.01))
        # merge to a t-digest with default compression
        self.assertOk(self.cmd("tdigest.merge", "to-tdigest-500{1}", "1","from-1{1}", "COMPRESSION", "500"))
        # assert min min/max have same result as quantile 0 and 1
        self.assertEqual(
            float(self.cmd("tdigest.max", "to-tdigest-500{1}")),
            float(self.cmd("tdigest.quantile", "to-tdigest-500{1}", 1.0)[0]),
        )
        self.assertEqual(
            float(self.cmd("tdigest.min", "to-tdigest-500{1}")),
            float(self.cmd("tdigest.quantile", "to-tdigest-500{1}", 0.0)[0]),
        )
        self.assertAlmostEqual(
            1.0, float(self.cmd("tdigest.quantile", "to-tdigest-500{1}", 0.01)[0]), 0.01
        )
        self.assertAlmostEqual(
            99.0, float(self.cmd("tdigest.quantile", "to-tdigest-500{1}", 0.99)[0]), 0.01
        )
        self.assertAlmostEqual(
            99.0, float(self.cmd("tdigest.quantile", "to-tdigest-500{1}", 0.01, 0.99)[1]), 0.01
        )
        expected = [1,50,95,99]
        res = self.cmd("tdigest.quantile", "to-tdigest-500{1}", 0.01, 0.5, 0.95, 0.99)
        self.env.debugPrint(res)
        for i in range(len(res)):
            self.assertAlmostEqual(expected[i], float(res[i]), 0.01)

    def test_negative_tdigest_merge(self):
        self.cmd('FLUSHALL')
        self.cmd("SET", "to-tdigest", "B")
        self.cmd("SET", "from-tdigest", "B")
        self.assertOk(self.cmd("tdigest.create", "from-1"))

        # WRONGTYPE
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.merge", "to-tdigest", "1", "from-tdigest"
        )
        # WRONGTYPE in the one of the inputs
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.merge", "to-tdigest", "2", "from-1", "from-tdigest"
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.merge", "to-tdigest", "2", "from-tdigest", "from-1"
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.merge", "to-tdigest", "1", "from-tdigest", "COMPRESSION", "a"
        )
        self.cmd("DEL", "to-tdigest")
        # arity lower
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.merge"
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.merge", "to-tdigest"
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.merge", "to-tdigest", "1"
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.merge", "to-tdigest", "1", "from-tdigest", "COMPRESSION"
        )
        # wrong keyword
        self.assertRaises(
            ResponseError,
            self.cmd,
            "tdigest.merge",
            "to-tdigest",
            "1",
            "from-tdigest",
            "extra-arg",
        )
        # arity upper
        self.assertRaises(
            ResponseError,
            self.cmd,
            "tdigest.merge",
            "to-tdigest",
            "1",
            "from-tdigest",
            "OVERRIDE",
            "extra-arg"
        )
        # numkeys needs to be a positive integer
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.merge", "to-tdigest", "-1", "from-tdigest"
        )
        # numkeys needs to be a positive integer
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.merge", "to-tdigest", "0", "from-tdigest"
        )
        # bad keyword
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.merge", "to-tdigest-500", "1","from-1",
                                                      "bad_keyword", "500"
        )
        # allocation of destination digest failed
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.merge", "to-tdigest", "1","from-1",
                                                      "COMPRESSION", "10000000000000000000"
        )

    def test_negative_tdigest_merge_crashes(self):
        # reported crash on merge to self where key does not exist
        self.cmd('FLUSHALL')
        for _ in range(1,1000):
            self.assertRaises(
                ResponseError, self.cmd,
                "tdigest.merge", "z", "5","z","z","z","z","z", "COMPRESSION", "3"
            )


    def test_negative_tdigest_merge_crashes_recursive(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('tdigest.create x{1} COMPRESSION 1000'))
        self.assertOk(self.cmd('tdigest.create y{1} COMPRESSION 1000'))
        self.assertOk(self.cmd('tdigest.add x{1} 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20'))
        self.assertOk(self.cmd('tdigest.add y{1} 101 102 103 104 105 106 107 108 109 110 111 112 113 114 115 116 117 118 119 120'))
        try:
            for x in range(1,500):
                self.cmd('tdigest.merge z{1} 5 x{1} y{1} x{1} y{1} x{1}')
                self.cmd('tdigest.merge z{1} 5 z{1} z{1} z{1} z{1} z{1}')
        except ResponseError as e:
            error_str = e.__str__()
            self.assertTrue("overflow detected" in error_str)


    def test_tdigest_min_max(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest"))
        # test for no datapoints first
        self.assertEqual('nan', self.cmd("tdigest.min", "tdigest"))
        self.assertEqual('nan', self.cmd("tdigest.max", "tdigest"))
        # insert datapoints into sketch
        for x in range(1, 101):
            self.assertOk(self.cmd("tdigest.add", "tdigest", x))
        # min/max
        self.assertEqual(100, float(self.cmd("tdigest.max", "tdigest")))
        self.assertEqual(1, float(self.cmd("tdigest.min", "tdigest")))

    def test_negative_tdigest_min_max(self):
        self.cmd('FLUSHALL')
        self.cmd("SET", "tdigest", "B")
        # WRONGTYPE
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.min", "tdigest"
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.max", "tdigest"
        )
        # key does not exist
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.min", "dont-exist"
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.max", "dont-exist"
        )

        self.cmd("DEL", "tdigest")
        self.assertOk(self.cmd("tdigest.create", "tdigest"))
        # arity lower
        self.assertRaises(ResponseError, self.cmd, "tdigest.min")
        self.assertRaises(ResponseError, self.cmd, "tdigest.max")
        # arity upper
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.min", "tdigest", 1
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.max", "tdigest", 1
        )

    def test_tdigest_quantile(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", 500))
        # insert datapoints into sketch
        for x in range(1, 10000):
            self.assertOk(self.cmd("tdigest.add", "tdigest", x * 0.01))
        # assert min min/max have same result as quantile 0 and 1
        self.assertEqual(
            float(self.cmd("tdigest.max", "tdigest")),
            float(self.cmd("tdigest.quantile", "tdigest", 1.0)[0]),
        )
        self.assertEqual(
            float(self.cmd("tdigest.min", "tdigest")),
            float(self.cmd("tdigest.quantile", "tdigest", 0.0)[0]),
        )
        self.assertAlmostEqual(
            1.0, float(self.cmd("tdigest.quantile", "tdigest", 0.01)[0]), 0.01
        )
        self.assertAlmostEqual(
            99.0, float(self.cmd("tdigest.quantile", "tdigest", 0.99)[0]), 0.01
        )
        self.assertAlmostEqual(
            99.0, float(self.cmd("tdigest.quantile", "tdigest", 0.01, 0.99)[1]), 0.01
        )
        expected = [1.0,50.0,95.0,99.0]
        res = self.cmd("tdigest.quantile", "tdigest", 0.01, 0.5, 0.95, 0.99)
        for i in range(len(res)):
            self.assertAlmostEqual(
                expected[i], float(res[i]), 0.01
            )
        # the reply provides the output percentiles in ordered manner
        expected = [95.0,99.0,1.0,50.0]
        res = self.cmd("tdigest.quantile", "tdigest", 0.95, 0.99, 0.01, 0.5)
        for i in range(len(res)):
            self.assertAlmostEqual(
                expected[i], float(res[i]), 0.01
            )

    def test_tdigest_single_observation(self):
        # https://github.com/RedisBloom/RedisBloom/issues/870
        # a sketch holding a single observation replied nan to
        # TDIGEST.QUANTILE, TDIGEST.CDF and TDIGEST.TRIMMED_MEAN
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest"))
        self.assertOk(self.cmd("tdigest.add", "tdigest", 123))
        for q in ["0", "0.5", "0.9", "1"]:
            self.assertEqual(
                123.0, float(self.cmd("tdigest.quantile", "tdigest", q)[0])
            )
        self.assertEqual(0.0, float(self.cmd("tdigest.cdf", "tdigest", 100)[0]))
        self.assertEqual(0.5, float(self.cmd("tdigest.cdf", "tdigest", 123)[0]))
        self.assertEqual(1.0, float(self.cmd("tdigest.cdf", "tdigest", 200)[0]))
        self.assertEqual(
            123.0, float(self.cmd("tdigest.trimmed_mean", "tdigest", 0.1, 0.9))
        )

    def test_negative_tdigest_quantile(self):
        self.cmd('FLUSHALL')
        self.cmd("SET", "tdigest", "B")
        # WRONGTYPE
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.quantile", "tdigest", 0.9
        )
        # key does not exist
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.quantile", "dont-exist", 0.9
        )
        self.cmd("DEL", "tdigest")
        self.assertOk(self.cmd("tdigest.create", "tdigest"))
        # arity lower
        self.assertRaises(ResponseError, self.cmd, "tdigest.quantile")
        # parsing
        self.assertRaises(
            ResponseError,
            self.cmd,
            "tdigest.quantile",
            "tdigest",
            1,
            "a",
        )
        # parsing quantile needs to be between [0,1]
        self.assertRaises(
            ResponseError,
            self.cmd,
            "tdigest.quantile",
            "tdigest",
            -0.5,
        )
        # parsing quantile needs to be between [0,1]
        self.assertRaises(
            ResponseError,
            self.cmd,
            "tdigest.quantile",
            "tdigest",
            1.1,
        )
        # parsing
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.quantile", "tdigest", "a"
        )

    def test_tdigest_cdf(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", 500))
        # insert datapoints into sketch
        for x in range(1, 100):
            self.assertOk(self.cmd("tdigest.add", "tdigest", x))

        self.assertAlmostEqual(
            0.01, float(self.cmd("tdigest.cdf", "tdigest", 1.0)[0]), 0.01
        )
        self.assertAlmostEqual(
            0.99, float(self.cmd("tdigest.cdf", "tdigest", 99.0)[0]), 0.01
        )
        self.assertAlmostEqual(
            0.99, float(self.cmd("tdigest.cdf", "tdigest", 1.0, 99.0)[1]), 0.01
        )
        self.assertAlmostEqual(
            0.01, float(self.cmd("tdigest.cdf", "tdigest", 99.0, 1.0)[1]), 0.01
        )

    def test_negative_tdigest_cdf(self):
        self.cmd('FLUSHALL')
        self.cmd("SET", "tdigest", "B")
        # WRONGTYPE
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.cdf", "tdigest", 0.9
        )
        # key does not exist
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.cdf", "dont-exist", 0.9
        )
        self.cmd("DEL", "tdigest")
        self.assertOk(self.cmd("tdigest.create", "tdigest"))
        # arity lower
        self.assertRaises(ResponseError, self.cmd, "tdigest.cdf")
        # parsing
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.cdf", "tdigest", "a"
        )
        # error with multi values
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.cdf", "tdigest", 1.0, 'foo'
        )

    def test_tdigest_rank(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", 500))
        # insert datapoints into sketch
        for x in range(0, 20):
            self.assertOk(self.cmd("tdigest.add", "tdigest", x))

        # -1 when value < value of the smallest observation
        self.assertEqual(-1, float(self.cmd("tdigest.rank", "tdigest", -1)[0]))
        # rank from cdf of min
        self.assertEqual(0, float(self.cmd("tdigest.rank", "tdigest", 0)[0]))
        # rank from cdf of max
        self.assertEqual(19, float(self.cmd("tdigest.rank", "tdigest", 19)[0]))
        # rank from cdf above max
        self.assertEqual(20, float(self.cmd("tdigest.rank", "tdigest", 20)[0]))
        # rank within [min,max]
        self.assertEqual(18, float(self.cmd("tdigest.rank", "tdigest", 18)[0]))
        self.assertEqual(10, float(self.cmd("tdigest.rank", "tdigest", 10)[0]))
        self.assertEqual(1, float(self.cmd("tdigest.rank", "tdigest", 1)[0]))
        # multiple inputs test
        self.assertEqual([-1,20,9], self.cmd("tdigest.rank", "tdigest", -20, 20, 9))

    def test_tdigest_revrank(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", 500))
        # insert datapoints into sketch
        for x in range(0, 20):
            self.assertOk(self.cmd("tdigest.add", "tdigest", x))

        # -1 when value > value of the largest observation
        self.assertEqual(-1, float(self.cmd("tdigest.revrank", "tdigest", 20)[0]))
        # rank from cdf of min
        self.assertEqual(19, float(self.cmd("tdigest.revrank", "tdigest", 0)[0]))
        # rank from cdf of max
        self.assertEqual(0, float(self.cmd("tdigest.revrank", "tdigest", 19)[0]))
        # rank from cdf above max
        self.assertEqual(-1, float(self.cmd("tdigest.revrank", "tdigest", 50)[0]))
        # rank within [min,max]
        self.assertEqual(1, float(self.cmd("tdigest.revrank", "tdigest", 18)[0]))
        self.assertEqual(9, float(self.cmd("tdigest.revrank", "tdigest", 10)[0]))
        self.assertEqual(18, float(self.cmd("tdigest.revrank", "tdigest", 1)[0]))
        # multiple inputs test
        self.assertEqual([-1,19,9], self.cmd("tdigest.revrank", "tdigest", 21, 0, 10))

    def test_tdigest_rank_and_revrank(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "t", "compression","1000"))
        self.assertOk(self.cmd('TDIGEST.ADD', 't', '1', '2', '2', '3', '3', '3', '4', '4', '4', '4', '5', '5', '5', '5', '5'))
        self.assertEqual([-1, 0, 2, 4, 8, 12, 15], self.cmd('TDIGEST.RANK', 't', '0', '1', '2', '3', '4', '5', '6'))
        self.assertEqual([15, 14, 13, 10, 7, 2, -1], self.cmd('TDIGEST.REVRANK', 't', '0', '1', '2', '3', '4', '5', '6'))

        # RANK on an empty sketch
        self.assertOk(self.cmd("tdigest.create", "empty"))
        self.assertEqual([-2, -2], self.cmd('TDIGEST.RANK', 'empty', '0', '1'))

        # REVRANK on an empty sketch
        self.assertEqual([-2, -2], self.cmd('TDIGEST.REVRANK', 'empty', '0', '1'))

        # round down RANK
        self.assertOk(self.cmd("tdigest.create", "s", "compression","1000"))
        self.assertOk(self.cmd('TDIGEST.ADD', 's', '10', '20', '30', '40', '50', '60'))
        self.assertEqual([-1, 0, 1, 2, 3, 4, 5, 6], self.cmd('TDIGEST.RANK', 's', '0', '10', '20', '30', '40', '50', '60', '70'))


    def test_negative_tdigest_rank(self):
        self.cmd('FLUSHALL')
        self.cmd("SET", "tdigest", "B")
        # WRONGTYPE
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.rank", "tdigest", 0.9
        )
        # key does not exist
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.rank", "dont-exist", 0.9
        )
        self.cmd("DEL", "tdigest")
        self.assertOk(self.cmd("tdigest.create", "tdigest"))
        # arity lower
        self.assertRaises(ResponseError, self.cmd, "tdigest.rank")
        # parsing
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.rank", "tdigest", math.nan
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.rank", "tdigest", "a", 0.9
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.rank", "tdigest", 1.5, "a"
        )

    def test_tdigest_byrank(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", 500))
        # insert datapoints into sketch
        self.assertOk(self.cmd("tdigest.add tdigest 1 2 3 4 5 6 7 8 9 10"))

        # rank 0 is precise ( equal to minimum )
        self.assertEqual(1, float(self.cmd("tdigest.byrank", "tdigest", 0)[0]))
        # rank of N
        self.assertEqual("inf", self.cmd("tdigest.byrank", "tdigest", 10)[0])
        # rank larger than total count
        self.assertEqual("inf", self.cmd("tdigest.byrank", "tdigest", 100)[0])
        # inverse rank of N-1: [1,10]
        self.assertEqual(10, float(self.cmd("tdigest.byrank", "tdigest", 9)[0]))

    def test_tdigest_byrevrank(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", 1000))
        # insert datapoints into sketch
        for x in range(1, 11):
            self.assertOk(self.cmd("tdigest.add", "tdigest", x))

        # inverse rank 0
        self.assertEqual(10, float(self.cmd("tdigest.byrevrank", "tdigest", 0)[0]))
        # inverse rank of N
        self.assertEqual("-inf", self.cmd("tdigest.byrevrank", "tdigest", 10)[0])
        # inverse rank larger than total count
        self.assertEqual("-inf", self.cmd("tdigest.byrevrank", "tdigest", 100)[0])
        # inverse rank of N-1
        self.assertEqual(1.0, float(self.cmd("tdigest.byrevrank", "tdigest", 9)[0]))

        # reset the sketch
        self.assertOk(self.cmd("tdigest.reset", "tdigest"))
        self.assertOk(self.cmd("TDIGEST.ADD tdigest 1 2 2 3 3 3 4 4 4 4 5 5 5 5 5"))
        expected_revrank = ['5', '5', '5', '5', '5', '4', '4', '4', '4', '3', '3', '3', '2', '2', '1', '-inf']
        expected_rank = ['inf', '5', '5', '5', '5', '5', '4', '4', '4', '4', '3', '3', '3', '2', '2', '1']
        self.assertEqual(expected_revrank, self.cmd("TDIGEST.BYREVRANK tdigest 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15"))
        self.assertEqual(expected_rank, self.cmd("TDIGEST.BYRANK tdigest 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0"))
        self.assertEqual(expected_rank [1:], expected_revrank[:-1])

    def test_negative_tdigest_byrank(self):
        self.cmd('FLUSHALL')
        self.cmd("SET", "tdigest", "B")
        # WRONGTYPE
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.byrank", "tdigest", 0.9
        )
        # key does not exist
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.byrank", "dont-exist", 0.9
        )
        self.cmd("DEL", "tdigest")
        self.assertOk(self.cmd("tdigest.create", "tdigest"))
        # arity lower
        self.assertRaises(ResponseError, self.cmd, "tdigest.byrank")
        # Error if rank is negative
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.byrank", "tdigest", -1
        )
        # Error if rank is not an integer
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.byrank", "tdigest", 0.5
        )
        # parsing
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.byrank", "tdigest", math.nan
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.byrank", "tdigest", "a", 0.9
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.byrank", "tdigest", 1.5, "a"
        )

    def test_tdigest_trimmed_mean(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", 500))
        # insert datapoints into sketch
        for x in range(0, 20):
            self.assertOk(self.cmd("tdigest.add", "tdigest", x))

        self.assertAlmostEqual(
            9.5, float(self.cmd("tdigest.trimmed_mean", "tdigest", 0.1,0.9)), 0.01
        )
        self.assertAlmostEqual(
            9.5, float(self.cmd("tdigest.trimmed_mean", "tdigest", 0.0,1.0)), 0.01
        )
        self.assertAlmostEqual(
            9.5, float(self.cmd("tdigest.trimmed_mean", "tdigest", 0.2,0.8)), 0.01
        )
        self.assertOk(self.cmd("tdigest.reset", "tdigest"))
        self.assertEqual(
            "nan", self.cmd("tdigest.trimmed_mean", "tdigest", 0.2,0.8)
        )
        # insert datapoints into sketch
        # given a high number of datapoints, the trimmed mean between a range on those datapoints
        # is approximate to the precise mean of the interval range
        for x in range(1, 10001):
            self.assertOk(self.cmd("tdigest.add", "tdigest", float(x)/1000.0))
        for x in range(1, 10):
            low_cut = float(x)/10.0
            high_cut = low_cut + 0.1
            self.assertAlmostEqual(
                x+0.5, float(self.cmd("tdigest.trimmed_mean", "tdigest", low_cut, high_cut)), 0.01
            )
        # simple confirmation that the when having:
        #   9 observations of value 1
        #   1 observation of value 5
        # and comparing vs sheets
        #      TRIMMEAN(G2:G11,0.2) we get 1.0
        #      TRIMMEAN(G2:G11,0.19) we get 1.4
        #      TRIMMEAN(G2:G11,0.10) we get 1.4
        #      TRIMMEAN(G2:G11,0.02) we get 1.4
        # if we replicate this on our trimmed_mean implementation we get the same results
        self.assertOk(self.cmd("tdigest.reset", "tdigest"))
        for x in range(0,9):
            self.assertOk(self.cmd("tdigest.add", "tdigest", 1.0))
        self.assertOk(self.cmd("tdigest.add", "tdigest", 5.0))
        self.assertAlmostEqual(
                1.0, float(self.cmd("tdigest.trimmed_mean", "tdigest", 0.10, 0.90)), 0.01
            )
        self.assertAlmostEqual(
                1.4, float(self.cmd("tdigest.trimmed_mean", "tdigest", 0.095, 0.905)), 0.01
            )
        self.assertAlmostEqual(
                1.4, float(self.cmd("tdigest.trimmed_mean", "tdigest", 0.05, 0.95)), 0.01
            )
        self.assertAlmostEqual(
                1.4, float(self.cmd("tdigest.trimmed_mean", "tdigest", 0.01, 0.99)), 0.01
            )

    def test_negative_tdigest_trimmed_mean(self):
        self.cmd('FLUSHALL')
        self.cmd("SET", "tdigest", "B")
        # WRONGTYPE
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.trimmed_mean", "tdigest", 0.9
        )
        # key does not exist
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.trimmed_mean", "dont-exist", 0.9
        )
        self.cmd("DEL", "tdigest")
        self.assertOk(self.cmd("tdigest.create", "tdigest"))
        # arity lower
        self.assertRaises(ResponseError, self.cmd, "tdigest.trimmed_mean")
        # arity upper
        self.assertRaises(
            ResponseError,
            self.cmd,
            "tdigest.trimmed_mean",
            "tdigest",
            1,
            1,
            1,
        )
        # parsing
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.trimmed_mean", "tdigest", "a", "a"
        )
        # low_cut_percentile and high_cut_percentile should be in [0,1]
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.trimmed_mean", "tdigest", "10.0", "20.0"
        )
        # low_cut_percentile should be lower than high_cut_percentile
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.trimmed_mean", "tdigest", "0.9", "0.1"
        )
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.trimmed_mean", "tdigest", "0.1", "0.1"
        )

    def test_tdigest_info(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest"))
        # insert datapoints into sketch
        for x in range(1, 101):
            self.assertOk(self.cmd("tdigest.add", "tdigest", x))
        td_info = parse_tdigest_info(self.cmd("tdigest.info", "tdigest"))
        # total weight
        self.assertEqual(
                100,
                int(td_info["Observations"]),
            )
        init_mem_usage = td_info["Memory usage"]
        # memory usage check
        self.assertTrue(
                init_mem_usage <= self.cmd("MEMORY", "USAGE", "tdigest"),
            )
        # independent of the datapoints this sketch has an invariant size after creation
        for x in range(1, 10001):
            self.assertOk(self.cmd("tdigest.add", "tdigest", x))
        td_info = parse_tdigest_info(self.cmd("tdigest.info", "tdigest"))
        self.assertEqual(
                init_mem_usage,
                td_info["Memory usage"],
            )
        previous_mem_usage = 0
        for compression in [100,200,300,400,500]:
            self.cmd('FLUSHALL')
            self.assertOk(self.cmd("tdigest.create", "tdigest", "compression", compression))
            td_info = parse_tdigest_info(self.cmd("tdigest.info", "tdigest"))
            current_mem_usage = td_info["Memory usage"]
            self.assertTrue(
                previous_mem_usage < current_mem_usage,
            )
            previous_mem_usage = current_mem_usage

    def test_negative_tdigest_info(self):
        self.cmd('FLUSHALL')
        self.cmd("SET", "tdigest", "B")
        # WRONGTYPE
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.info", "tdigest"
        )
        # dont exist
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.info", "dont-exist"
        )
        self.cmd("DEL", "tdigest")
        self.assertOk(self.cmd("tdigest.create", "tdigest"))
        # arity lower
        self.assertRaises(ResponseError, self.cmd, "tdigest.info")
        # arity upper
        self.assertRaises(
            ResponseError, self.cmd, "tdigest.info", "tdigest", 1
        )

    def test_save_load(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd("tdigest.create", "tdigest"))
        # insert datapoints into sketch
        for _ in range(1, 101):
            self.assertOk(self.cmd("tdigest.add", "tdigest", 1.0))
        self.assertEqual('OK', self.cmd("SAVE"))
        mem_usage_prior_restart = self.cmd("MEMORY", "USAGE", "tdigest")
        tdigest_min = self.cmd("tdigest.min", "tdigest")
        tdigest_max = self.cmd("tdigest.max", "tdigest")
        self.restart_and_reload()
        # Native version 1 preserves the buffer rather than compressing during SAVE.
        self.assertEqual(1, self.cmd("EXISTS", "tdigest"))
        self.assertEqual(
            100,
            float(
                parse_tdigest_info(self.cmd("tdigest.info", "tdigest"))["Unmerged weight"]
            ),
        )
        mem_usage_after_restart = self.cmd("MEMORY", "USAGE", "tdigest")
        self.assertEqual(mem_usage_prior_restart, mem_usage_after_restart)
        self.assertEqual(tdigest_min, self.cmd("tdigest.min", "tdigest"))
        self.assertEqual(tdigest_max, self.cmd("tdigest.max", "tdigest"))

    def test_insufficient_memory(self):
        if os.environ.get('SANITIZER') != None:
            self.env.skip()
        self.cmd("FLUSHALL")
        self.env.expect('tdigest.create', 'k', 'compression', 100000000000000000).error().contains('allocation failed')

    def test_rdb_load_oob_guard(self):
        self.cmd('FLUSHALL')
        rdb_payload = b'\x07\x81L2\x12\xf96\x0f\x10\x00\x04\x00\x00\x00\x00\x00\x00(@\x04\x00\x00\x00\x00\x00\x00\xf0?\x04\x00\x00\x00\x00\x00\x00\xf0?\x02\x01\x02@a\x02\x01\x02\x01\x04\x00\x00\x00\x00\x00\x00\xf0?\x04\x00\x00\x00\x00\x00\x00\xf0?\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04CCCCCCCC\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x04AAAAAAAA\x00\xff\x0c\x008\x9c\x969\x91:\xcc5'
        self.env.expect('RESTORE', "key", 0, rdb_payload, 'REPLACE').error().contains('Bad data format')
