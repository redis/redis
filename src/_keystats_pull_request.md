Keystats

To do:
- How to check if the terminal supports moving the cursor up. Is it portable?
- Use a Linter
- bytesToHuman unit test
- methods comment on top (doc)

Display issues:
- stderr in between refresh is ugly but works
- If the terminal is not high enough the refresh will loose the introduction text and can leave a trail 

Tests:
- Tested with Modules, empty types, and empty DB
- Tested in Mac (arm) and Ubuntu 22 (arm)
- Tested with --keystats-samples 0
- Tested with -i 0.1
- Does not show when no keys or 0 size
- Do I need to add to tests/integration/redis-cli.tcl? I don't see anything for memkey or bigkeys

Improvements:
- Add shard or slot for the keys?
- Show if the keys are volatile or not?
- Slot distribution between (avg, max min, for number of keys and size)
- Align all columns as much as possible? Parsable?
- RAW works if sent to a file, should we do a JSON or CSV as well?
- Check accuracy of memory usage and maybe add a disclaimer
- Add bigkeys commands for modules (right now NULL but might not make sense)
- should I keep the '"keyname"' instead of "keyname" due to parsing? (')
- Add findHotKeys info?

// YLB refresh issue if the terminal window is not wide or high enough
// YLB TODO trim the key name if more than xx char? Though we will not get the entire name (memkeys does not trim)
// YLB TODO Lint? Unit Test? valgrind to check for memory leak?
// YLB display issue if the window is not large or high enough, check cursor move work everywhere
// YLB TODO have a spinning cursor to show it is working (on slow db connection it might look like we are doing nothing)

---
Pull Request

Added `--keystats` and `--keystats-samples <n>` commands.

The new command combines memkeys and bigkeys with additional distribution data.
We often run memkeys and bigkeys one after the other. It will be convenient to just have one command.
Distribution and top 10 key sizes is useful when we have multiple keys taking a lot of memory.

Like for memkeys and bigkeys, we can use `-i <n>` and Ctrl-C to interrupt the scan loop to show statistics on the keys sampled so far.
We can use some optional parameters `--cursor <n>` to restart after an interruption of the scan, and `--top <n>` to change the number of top key sizes (10 by default).

Implemented a fix for the `--memkeys-samples 0` issue in order to use `--keystats-samples 0`.

Used hdr_histogram for the key size distribution.
For key length, hdr_histogram seemed overkill and preset ranges were used.

The memory used by keystats with hdr_histogram is around 7MB (compared to 3MB for memkeys or bigkeys).

Execution time is equivalent to adding memkeys and bigkeys time. Each Scan loop is having more commands (key type, key size, key length/cardinality).

We can redirect the output to a file.

No unit tests or integration tests (not sure if we should).

Limitation:
- As the information printed during the loop is refreshed (moving cursor up), stderr and information not fitting in the terminal window (width or height) might create some refresh issues.

Possible improvements:
- Thousand separator for better redeability (/usr/bin/printf "%'d\n" 12345).

Comments:
- config.top_sizes_limit could be used globally like config.count, but is passed as parameter to be consistent with config.memkeys_samples.
- Not sure if we should move some utility functions to cli-common.c?