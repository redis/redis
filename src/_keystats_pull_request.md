Keystats Pull Request

Added `--keystats` and `--keystats-samples <n>` commands.

The new command combines memkeys and bigkeys with additional distribution data.
We often run memkeys and bigkeys one after the other. It will be convenient to just have one command.
Distribution and top 10 key sizes are useful when we have multiple keys taking a lot of memory.

Like for memkeys and bigkeys, we can use `-i <n>` and Ctrl-C to interrupt the scan loop. It will still show the statistics on the keys sampled so far.
We can use two new optional parameters:
- `--cursor <n>` to restart after an interruption of the scan.
- `--top <n>` to change the number of top key sizes (10 by default).

Implemented a fix for the `--memkeys-samples 0` issue in order to use `--keystats-samples 0`.

Used hdr_histogram for the key size distribution.
For key length, hdr_histogram seemed overkill and preset ranges were used.

The memory used by keystats with hdr_histogram is around 7MB (compared to 3MB for memkeys or bigkeys).

Execution time is somewhat equivalent to adding memkeys and bigkeys time. Each scan loop is having more commands (key type, key size, key length/cardinality).

We can redirect the output to a file. In that case, no color or text refresh will happen.

Limitation:
- As the information printed during the loop is refreshed (moving cursor up), stderr and information not fitting in the terminal window (width or height) might create some refresh issues.

Improvements:
- Thousand separator for better redeability (/usr/bin/printf "%'d\n" 12345).

Comments:
- config.top_sizes_limit could be used globally like config.count, but it is passed as parameter to be consistent with config.memkeys_samples.
- Not sure if we should move some utility functions to cli-common.c.
