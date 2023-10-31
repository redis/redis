#!/usr/bin/env python3

import math

# Must match TICKER_GEOM_NBITS
lg_table_size = 6
table_size = 2**lg_table_size
byte_max = 255
mul = math.floor(-byte_max/math.log(1 / table_size))
values = [round(-mul * math.log(i / table_size))
	for i in range(1, table_size+1)]
print("mul =", mul)
print("values:")
for i in range(table_size // 8):
	print(", ".join((str(x) for x in values[i*8 : i*8 + 8])))
