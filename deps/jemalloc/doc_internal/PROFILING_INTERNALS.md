# jemalloc profiling
This describes the mathematical basis behind jemalloc's profiling implementation, as well as the implementation tricks that make it effective. Historically, the jemalloc profiling design simply copied tcmalloc's. The implementation has since diverged, due to both the desire to record additional information, and to correct some biasing bugs.

Note: this document is markdown with embedded LaTeX; different markdown renderers may not produce the expected output.  Viewing with `pandoc -s PROFILING_INTERNALS.md -o PROFILING_INTERNALS.pdf` is recommended.

## Some tricks in our implementation toolbag

### Sampling
Recording our metadata is quite expensive; we need to walk up the stack to get a stack trace. On top of that, we need to allocate storage to record that stack trace, and stick it somewhere where a profile-dumping call can find it. That call might happen on another thread, so we'll probably need to take a lock to do so. These costs are quite large compared to the average cost of an allocation. To manage this, we'll only sample some fraction of allocations. This will miss some of them, so our data will be incomplete, but we'll try to make up for it. We can tune our sampling rate to balance accuracy and performance.

### Fast Bernoulli sampling
Compared to our fast paths, even a `coinflip(p)` function can be quite expensive. Having to do a random-number generation and some floating point operations would be a sizeable relative cost. However (as pointed out in [[Vitter, 1987](https://dl.acm.org/doi/10.1145/23002.23003)]), if we can orchestrate our algorithm so that many of our `coinflip` calls share their parameter value, we can do better. We can sample from the geometric distribution, and initialize a counter with the result. When the counter hits 0, the `coinflip` function returns true (and reinitializes its internal counter).
This can let us do a random-number generation once per (logical) coinflip that comes up heads, rather than once per (logical) coinflip. Since we expect to sample relatively rarely, this can be a large win.

### Fast-path / slow-path thinking
Most programs have a skewed distribution of allocations. Smaller allocations are much more frequent than large ones, but shorter lived and less common as a fraction of program memory. "Small" and "large" are necessarily sort of fuzzy terms, but if we define "small" as "allocations jemalloc puts into slabs" and "large" as the others, then it's not uncommon for small allocations to be hundreds of times more frequent than large ones, but take up around half the amount of heap space as large ones. Moreover, small allocations tend to be much cheaper than large ones (often by a factor of 20-30): they're more likely to hit in thread caches, less likely to have to do an mmap, and cheaper to fill (by the user) once the allocation has been returned.

## An unbiased estimator of space consumption from (almost) arbitrary sampling strategies
Suppose we have a sampling strategy that meets the following criteria:

  - One allocation being sampled is independent of other allocations being sampled.
  - Each allocation has a non-zero probability of being sampled.

We can then estimate the bytes in live allocations through some particular stack trace as:

$$ \sum_i S_i I_i \frac{1}{\mathrm{E}[I_i]} $$

where the sum ranges over some index variable of live allocations from that stack, $S_i$ is the size of the $i$'th allocation, and $I_i$ is an indicator random variable for whether or not the $i'th$ allocation is sampled. $S_i$ and $\mathrm{E}[I_i]$ are constants (the program allocations are fixed; the random variables are the sampling decisions), so taking the expectation we get

$$ \sum_i S_i \mathrm{E}[I_i] \frac{1}{\mathrm{E}[I_i]}.$$

This is of course $\sum_i S_i$, as we want (and, a similar calculation could be done for allocation counts as well).
This is a fairly general strategy; note that while we require that sampling decisions be independent of one another's outcomes, they don't have to be independent of previous allocations, total bytes allocated, etc. You can imagine strategies that:

  - Sample allocations at program startup at a higher rate than subsequent allocations
  - Sample even-indexed allocations more frequently than odd-indexed ones (so long as no allocation has zero sampling probability)
  - Let threads declare themselves as high-sampling-priority, and sample their allocations at an increased rate.

These can all be fit into this framework to give an unbiased estimator.

## Evaluating sampling strategies
Not all strategies for picking allocations to sample are equally good, of course. Among unbiased estimators, the lower the variance, the lower the mean squared error. Using the estimator above, the variance is:

$$
\begin{aligned}
& \mathrm{Var}[\sum_i S_i I_i \frac{1}{\mathrm{E}[I_i]}]  \\
=& \sum_i \mathrm{Var}[S_i I_i \frac{1}{\mathrm{E}[I_i]}] \\
=& \sum_i \frac{S_i^2}{\mathrm{E}[I_i]^2} \mathrm{Var}[I_i] \\
=& \sum_i \frac{S_i^2}{\mathrm{E}[I_i]^2} \mathrm{Var}[I_i] \\
=& \sum_i \frac{S_i^2}{\mathrm{E}[I_i]^2} \mathrm{E}[I_i](1 - \mathrm{E}[I_i]) \\
=& \sum_i S_i^2 \frac{1 - \mathrm{E}[I_i]}{\mathrm{E}[I_i]}.
\end{aligned}
$$

We can use this formula to compare various strategy choices. All else being equal, lower-variance strategies are better.

## Possible sampling strategies
Because of the desire to avoid the fast-path costs, we'd like to use our Bernoulli trick if possible. There are two obvious counters to use: a coinflip per allocation, and a coinflip per byte allocated.

### Bernoulli sampling per-allocation
An obvious strategy is to pick some large $N$, and give each allocation a $1/N$ chance of being sampled. This would let us use our Bernoulli-via-Geometric trick. Using the formula from above, we can compute the variance as:

$$ \sum_i S_i^2 \frac{1 - \frac{1}{N}}{\frac{1}{N}}  = (N-1) \sum_i S_i^2.$$

That is, an allocation of size $Z$ contributes a term of $(N-1)Z^2$ to the variance.

### Bernoulli sampling per-byte
Another option we have is to pick some rate $R$, and give each byte a $1/R$ chance of being picked for sampling (at which point we would sample its contained allocation). The chance of an allocation of size $Z$ being sampled, then, is

$$1-(1-\frac{1}{R})^{Z}$$

and an allocation of size $Z$ contributes a term of

$$Z^2 \frac{(1-\frac{1}{R})^{Z}}{1-(1-\frac{1}{R})^{Z}}.$$

In practical settings, $R$ is large, and so this is well-approximated by

$$Z^2 \frac{e^{-Z/R}}{1 - e^{-Z/R}} .$$

Just to get a sense of the dynamics here, let's look at the behavior for various values of $Z$. When $Z$ is small relative to $R$, we can use $e^z \approx 1 + x$, and conclude that the variance contributed by a small-$Z$ allocation is around

$$Z^2 \frac{1-Z/R}{Z/R} \approx RZ.$$

When $Z$ is comparable to $R$, the variance term is near $Z^2$ (we have $\frac{e^{-Z/R}}{1 - e^{-Z/R}} = 1$ when $Z/R = \ln 2 \approx 0.693$). When $Z$ is large relative to $R$, the variance term goes to zero.

## Picking a sampling strategy
The fast-path/slow-path dynamics of allocation patterns point us towards the per-byte sampling approach:

  - The quadratic increase in variance per allocation in the first approach is quite costly when heaps have a non-negligible portion of their bytes in those allocations, which is practically often the case.
  - The Bernoulli-per-byte approach shifts more of its samples towards large allocations, which are already a slow-path.
  - We drive several tickers (e.g. tcache gc) by bytes allocated, and report bytes-allocated as a user-visible statistic, so we have to do all the necessary bookkeeping anyways.

Indeed, this is the approach we use in jemalloc. Our heap dumps record the size of the allocation and the sampling rate $R$, and jeprof unbiases by dividing by $1 - e^{-Z/R}$.  The framework above would suggest dividing by $1-(1-1/R)^Z$; instead, we use the fact that $R$ is large in practical situations, and so $e^{-Z/R}$ is a good approximation (and faster to compute).  (Equivalently, we may also see this as the factor that falls out from viewing sampling as a Poisson process directly).

## Consequences for heap dump consumers
Using this approach means that there are a few things users need to be aware of.

### Stack counts are not proportional to allocation frequencies
If one stack appears twice as often as another, this by itself does not imply that it allocates twice as often. Consider the case in which there are only two types of allocating call stacks in a program. Stack A allocates 8 bytes, and occurs a million times in a program. Stack B allocates 8 MB, and occurs just once in a program. If our sampling rate $R$ is about 1MB, we expect stack A to show up about 8 times, and stack B to show up once. Stack A isn't 8 times more frequent than stack B, though; it's a million times more frequent.

### Aggregation must be done after unbiasing samples
Some tools manually parse heap dump output, and aggregate across stacks (or across program runs) to provide wider-scale data analyses. When doing this aggregation, though, it's important to unbias-and-then-sum, rather than sum-and-then-unbias. Reusing our example from the previous section: suppose we collect heap dumps of the program from a million machines. We then have 8 million occurs of stack A (each of 8 bytes), and a million occurrences of stack B (each of 8 MB). If we sum first, we'll attribute 64 MB to stack A, and 8 TB to stack B. Unbiasing changes these numbers by an infinitesimal amount, so that sum-then-unbias dramatically underreports the amount of memory allocated by stack A.

## An avenue for future exploration
While the framework we laid out above is pretty general, as an engineering decision we're only interested in fairly simple approaches (i.e. ones for which the chance of an allocation being sampled depends only on its size). Our job is then: for each size class $Z$, pick a probability $p_Z$ that an allocation of that size will be sampled. We made some handwave-y references to statistical distributions to justify our choices, but there's no reason we need to pick them that way. Any set of non-zero probabilities is a valid choice.
The real limiting factor in our ability to reduce estimator variance is that fact that sampling is expensive; we want to make sure we only do it on a small fraction of allocations. Our goal, then, is to pick the $p_Z$ to minimize variance given some maximum sampling rate $P$. If we define $a_Z$ to be the fraction of allocations of size $Z$, and $l_Z$ to be the fraction of allocations of size $Z$ still alive at the time of a heap dump, then we can phrase this as an optimization problem over the choices of $p_Z$:

Minimize

$$ \sum_Z Z^2 l_Z \frac{1-p_Z}{p_Z} $$

subject to

$$ \sum_Z a_Z p_Z \leq P $$

Ignoring a term that doesn't depend on $p_Z$, the objective is minimized whenever

$$ \sum_Z Z^2 l_Z \frac{1}{p_Z} $$

is. For a particular program, $l_Z$ and $a_Z$ are just numbers that can be obtained (exactly) from existing stats introspection facilities, and we have a fairly tractable convex optimization problem (it can be framed as a second-order cone program). It would be interesting to evaluate, for various common allocation patterns, how well our current strategy adapts. Do our actual choices for $p_Z$ closely correspond to the optimal ones? How close is the variance of our choices to the variance of the optimal strategy?
You can imagine an implementation that actually goes all the way, and makes $p_Z$ selections a tuning parameter. I don't think this is a good use of development time for the foreseeable future; but I do wonder about the answers to some of these questions.

## Implementation realities

The nice story above is at least partially a lie. Initially, jeprof (copying its logic from pprof)  had the sum-then-unbias error described above.  The current version of jemalloc does the unbiasing step on a per-allocation basis internally, so that we're always tracking what the unbiased numbers "should" be.  The problem is, actually surfacing those unbiased numbers would require a breaking change to jeprof (and the various already-deployed tools that have copied its logic). Instead, we use a little bit more trickery. Since we know at dump time the numbers we want jeprof to report, we simply choose the values we'll output so that the jeprof numbers will match the true numbers.  The math is described in `src/prof_data.c` (where the only cleverness is a change of variables that lets the exponentials fall out).

This has the effect of making the output of jeprof (and related tools) correct, while making its inputs incorrect.  This can be annoying to human readers of raw profiling dump output.
