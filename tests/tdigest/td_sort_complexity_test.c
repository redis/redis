/*
 * Complexity regression for the centroid sort (MOD-17228).
 *
 * A correctness-only test cannot tell an O(n log n) sort from an O(n^2) one, and the ordinary
 * unit test auto-compresses in small batches so it never sorts one large partition. This test
 * compiles the library with TD_INSTRUMENT_SORT (key-comparison and heapsort-fallback counters)
 * and asserts the two properties that give td_qsort its worst-case guarantee, each driven
 * through the real sort by the input that actually defeats a sort lacking it, across multiple
 * sizes so a quadratic implementation cannot slip under a single hand-picked bound:
 *
 *   Part A - duplicate-heavy input (the reported DoS). N identical values is the MOD-17228
 *            vector: the pre-PR central-pivot Lomuto sort peels one element per level and does
 *            exactly n^2/2 comparisons (~5e9 at n=1e5, a ~98 s server freeze). The 3-way
 *            partition collapses the equal run in a single pass -> exactly 2n comparisons.
 *            We assert a strict LINEAR bound, which a revert to any sort without equal-run
 *            handling misses by three-plus orders of magnitude.
 *
 *   Part B - the heapsort fallback, through the FULL td_qsort() path. A median-of-three
 *            quicksort has an adversarial distinct permutation that forces Theta(n^2); the
 *            depth-limit -> heapsort fallback is what caps it at O(n log n). We generate that
 *            permutation with McIlroy's quicksort adversary (see gen_killer) run against a
 *            faithful mirror of this introsort's pivot/partition sequence, feed it through the
 *            real td_compress()/td_qsort(), and assert three things across sizes: the heapsort
 *            fallback is actually reached (td_sort_heap_fallbacks > 0), the comparison count
 *            stays within an O(n log n) bound (measured ~5.7 n log2 n; without the fallback the
 *            same input is ~n^2/2, i.e. ~3000 n log2 n at n=1e5), and the normalized ratio
 *            stays flat. Removing or weakening the fallback fails both the fallback-reached and
 *            the bound assertion.
 *
 * Numbers above are measured, not assumed; the asserted constants leave generous margin over
 * them while staying far below quadratic.
 */
#ifndef TD_INSTRUMENT_SORT
/* Also set by target_compile_definitions() in tests/CMakeLists.txt; guard so a standalone
 * compile still instruments without redefining the macro (which -Werror would reject). */
#define TD_INSTRUMENT_SORT 1
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "tdigest.c" /* brings in the static sort helpers + instrumentation counters */

static int failures = 0;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: ");                                                             \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fprintf(stderr, "\n");                                                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ---------- Part A ---------- */

/* Sort n identical values in a single compress and return the comparison count. */
static double compares_all_equal(int n) {
    /* cap = 6n + 10 > n, so all n points buffer and one td_compress() sorts them together. */
    td_histogram_t *h = td_new((double)n);
    if (h == NULL) {
        fprintf(stderr, "allocation failed at n=%d\n", n);
        exit(1);
    }
    for (int i = 0; i < n; ++i) {
        if (td_add(h, 42.0, 1) != 0) {
            fprintf(stderr, "td_add failed at %d\n", i);
            exit(1);
        }
    }
    if (h->unmerged_nodes != n) {
        fprintf(stderr, "expected one big compress, but auto-compress ran (unmerged=%d)\n",
                h->unmerged_nodes);
        exit(1);
    }
    td_sort_comparisons = 0;
    if (td_compress(h) != 0) {
        fprintf(stderr, "compress failed at n=%d\n", n);
        exit(1);
    }
    const double c = (double)td_sort_comparisons;
    for (int i = 1; i < h->merged_nodes; ++i) {
        CHECK(h->nodes_mean[i - 1] <= h->nodes_mean[i], "A: centroids not sorted at %d (n=%d)", i,
              n);
    }
    td_free(h);
    return c;
}

/* ---------- Part B: McIlroy quicksort adversary tailored to this introsort ----------
 *
 * The adversary keeps every key "gas" (unassigned) and freezes one only when a comparison
 * forces it, always making the pivot just chosen an extreme -> maximally unbalanced partitions.
 * It must be driven by the SAME comparison sequence as the target sort, so mirror_qsort below is
 * a faithful copy of td_introsort's median-of-three + 3-way partition + recurse-smaller-side
 * logic (without the depth limit, so generation elicits the quadratic path). The resulting value
 * assignment is the killer permutation for the real td_qsort(). If td_introsort's pivot or
 * partition strategy changes, this mirror must change with it. */
static int *g_val;   /* assigned key per identity, or g_gas if still unassigned */
static int g_gas;    /* sentinel meaning "unassigned" */
static int g_nsolid; /* number of frozen keys */
static int g_cand;   /* current pivot candidate */

static int adv_cmp(int x, int y) { /* <0 if key(x)<key(y), >0 if greater, 0 if equal */
    if (g_val[x] == g_gas && g_val[y] == g_gas) {
        if (x == g_cand) {
            g_val[x] = g_nsolid++;
        } else {
            g_val[y] = g_nsolid++;
        }
    }
    if (g_val[x] == g_gas) {
        g_cand = x;
        return 1;
    }
    if (g_val[y] == g_gas) {
        g_cand = y;
        return -1;
    }
    return g_val[x] - g_val[y];
}

static void idx_swap(int *a, int i, int j) {
    const int t = a[i];
    a[i] = a[j];
    a[j] = t;
}

/* mirror of td_insertion_sort / td_median3 / td_introsort (see TD_INSORT_THRESHOLD). */
static void mirror_insertion(int *a, int lo, int hi) {
    for (int i = lo + 1; i <= hi; i++) {
        const int m = a[i];
        int j = i - 1;
        while (j >= lo && adv_cmp(m, a[j]) < 0) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = m;
    }
}

static int mirror_median3(int *a, int lo, int mid, int hi) {
    if (adv_cmp(a[mid], a[lo]) < 0) {
        idx_swap(a, lo, mid);
    }
    if (adv_cmp(a[hi], a[lo]) < 0) {
        idx_swap(a, lo, hi);
    }
    if (adv_cmp(a[hi], a[mid]) < 0) {
        idx_swap(a, mid, hi);
    }
    return a[mid];
}

static void mirror_qsort(int *a, int lo, int hi) {
    while (hi - lo > TD_INSORT_THRESHOLD) {
        const int mid = lo + (hi - lo) / 2;
        const int pivot = mirror_median3(a, lo, mid, hi);
        int lt = lo;
        int i = lo;
        int gt = hi;
        while (i <= gt) {
            const int c = adv_cmp(a[i], pivot);
            if (c < 0) {
                idx_swap(a, i, lt);
                lt++;
                i++;
            } else if (c > 0) {
                idx_swap(a, i, gt);
                gt--;
            } else {
                i++;
            }
        }
        const int left_size = lt - lo;
        const int right_size = hi - gt;
        if (left_size < right_size) {
            if (left_size > 1) {
                mirror_qsort(a, lo, lt - 1);
            }
            lo = gt + 1;
        } else {
            if (right_size > 1) {
                mirror_qsort(a, gt + 1, hi);
            }
            hi = lt - 1;
        }
    }
    mirror_insertion(a, lo, hi);
}

/* Fill out[0..n-1] with the killer permutation for the real td_qsort. */
static void gen_killer(int n, double *out) {
    g_val = (int *)calloc((size_t)n, sizeof(int));
    int *a = (int *)calloc((size_t)n, sizeof(int));
    if (g_val == NULL || a == NULL) {
        fprintf(stderr, "allocation failed at n=%d\n", n);
        exit(1);
    }
    g_gas = n;
    g_nsolid = 0;
    g_cand = 0;
    for (int i = 0; i < n; i++) {
        g_val[i] = g_gas;
        a[i] = i;
    }
    mirror_qsort(a, 0, n - 1);
    for (int i = 0; i < n; i++) {
        if (g_val[i] == g_gas) { /* never compared: assign any remaining rank */
            g_val[i] = g_nsolid++;
        }
        out[i] = (double)g_val[i];
    }
    free(a);
    free(g_val);
    g_val = NULL;
}

/* Sort the killer through the real td_compress()/td_qsort(); report comparisons + fallbacks. */
static double compares_killer(int n, unsigned long long *fallbacks_out) {
    double *killer = (double *)calloc((size_t)n, sizeof(double));
    if (killer == NULL) {
        fprintf(stderr, "allocation failed at n=%d\n", n);
        exit(1);
    }
    gen_killer(n, killer);

    td_histogram_t *h = td_new((double)n);
    if (h == NULL) {
        fprintf(stderr, "allocation failed at n=%d\n", n);
        exit(1);
    }
    for (int i = 0; i < n; ++i) {
        if (td_add(h, killer[i], 1) != 0) {
            fprintf(stderr, "td_add failed at %d\n", i);
            exit(1);
        }
    }
    free(killer);
    if (h->unmerged_nodes != n) {
        fprintf(stderr, "expected one big compress, but auto-compress ran (unmerged=%d)\n",
                h->unmerged_nodes);
        exit(1);
    }
    td_sort_comparisons = 0;
    td_sort_heap_fallbacks = 0;
    if (td_compress(h) != 0) {
        fprintf(stderr, "compress failed at n=%d\n", n);
        exit(1);
    }
    const double c = (double)td_sort_comparisons;
    *fallbacks_out = td_sort_heap_fallbacks;
    for (int i = 1; i < h->merged_nodes; ++i) {
        CHECK(h->nodes_mean[i - 1] <= h->nodes_mean[i], "B: centroids not sorted at %d (n=%d)", i,
              n);
    }
    td_free(h);
    return c;
}

int main(void) {
    /* Part A is O(n) so it runs at large sizes. Part B's killer GENERATION runs the adversarial
     * mirror sort, which is intentionally O(n^2), so it uses modest sizes; the fallback engages
     * and the bounds separate quadratic from O(n log n) well before n gets large. */
    const int a_sizes[] = {25000, 50000, 100000, 200000};
    const int b_sizes[] = {3000, 6000, 12000, 24000};
    const int na = (int)(sizeof(a_sizes) / sizeof(a_sizes[0]));
    const int nb = (int)(sizeof(b_sizes) / sizeof(b_sizes[0]));

    /* Part A: duplicate-heavy input must stay LINEAR (3-way partition). New sort = 2n; the
     * pre-PR Lomuto sort = n^2/2. Bound 8n leaves 4x margin and is far below quadratic. */
    printf("Part A - duplicate-heavy (MOD-17228 DoS), must be linear:\n");
    for (int i = 0; i < na; ++i) {
        const int n = a_sizes[i];
        const double c = compares_all_equal(n);
        const double linear_bound = 8.0 * (double)n;
        const double quadratic = 0.5 * (double)n * (double)n;
        printf("  n=%7d  cmps=%12.0f  c/n=%5.2f  bound(8n)=%.0f  (pre-PR ~n^2/2=%.0f)\n", n, c,
               c / n, linear_bound, quadratic);
        CHECK(c <= linear_bound, "A: n=%d comparisons %.0f exceed linear bound %.0f (quadratic?)",
              n, c, linear_bound);
    }

    /* Part B: adversarial distinct permutation through the FULL td_qsort() path. The fallback
     * must be reached and cap the work at O(n log n) (measured ~5.7 n log2 n); the SAME input
     * without the fallback is ~n^2/2 (~3000 n log2 n at n=1e5). Bound 12 n log2 n. */
    printf("Part B - median-of-3 killer through full td_qsort(), fallback must engage:\n");
    double max_ratio = 0.0;
    double min_ratio = 1e300;
    for (int i = 0; i < nb; ++i) {
        const int n = b_sizes[i];
        unsigned long long fallbacks = 0;
        const double c = compares_killer(n, &fallbacks);
        const double nlogn = (double)n * log2((double)n);
        const double ratio = c / nlogn;
        if (ratio > max_ratio) {
            max_ratio = ratio;
        }
        if (ratio < min_ratio) {
            min_ratio = ratio;
        }
        printf("  n=%7d  cmps=%12.0f  c/(n*log2n)=%5.3f  fallbacks=%llu  bound(12*n*log2n)=%.0f\n",
               n, c, ratio, fallbacks, 12.0 * nlogn);
        CHECK(fallbacks > 0, "B: n=%d heapsort fallback was never reached (killer ineffective?)",
              n);
        CHECK(c <= 12.0 * nlogn, "B: n=%d comparisons %.0f exceed O(n log n) bound %.0f", n, c,
              12.0 * nlogn);
    }
    /* Flatness: for O(n log n) the ratio is ~constant; a quadratic path would grow it ~n/log n. */
    CHECK(max_ratio <= 2.0 * min_ratio, "B: normalized comparison ratio not flat (%.3f..%.3f)",
          min_ratio, max_ratio);

    if (failures == 0) {
        printf("OK\n");
        return 0;
    }
    fprintf(stderr, "%d complexity check(s) failed\n", failures);
    return 1;
}

void _serverAssert(const char *e, const char *f, int line) {
    fprintf(stderr,"%s:%d: %s\n",f,line,e); abort();
}
