#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>

int decr_every = 1;
int keyspace_size = 1000000;
time_t switch_after = 30; /* Switch access pattern after N seconds. */

struct entry {
    /* Field that the LFU Redis implementation will have (we have
     * 24 bits of total space in the object->lru field). */
    uint8_t counter;    /* Logarithmic counter. */
    uint16_t decrtime;  /* (Reduced precision) time of last decrement. */

    /* Fields only useful for visualization. */
    uint64_t hits;      /* Number of real accesses. */
    time_t ctime;       /* Key creation time. */
};

#define to_16bit_minutes(x) ((x/60) & 65535)
#define LFU_INIT_VAL 5

/* Compute the difference in minutes between two 16 bit minutes times
 * obtained with to_16bit_minutes(). Since they can wrap around if
 * we detect the overflow we account for it as if the counter wrapped
 * a single time. */
uint16_t minutes_diff(uint16_t now, uint16_t prev) {
    if (now >= prev) return now-prev;
    return 65535-prev+now;
}

/* Increment a counter logarithmically: the greatest is its value, the
 * less likely is that the counter is really incremented.
 * The maximum value of the counter is saturated at 255. */
uint8_t log_incr(uint8_t counter) {
    if (counter == 255) return counter;
    double r = (double)rand()/RAND_MAX;
    double baseval = counter-LFU_INIT_VAL;
    if (baseval < 0) baseval = 0;
    double limit = 1.0/(baseval*10+1);
    if (r < limit) counter++;
    return counter;
}

/* Simulate an access to an entry. */
void access_entry(struct entry *e) {
    e->counter = log_incr(e->counter);
    e->hits++;
}

/* Return the entry LFU value and as a side effect decrement the
 * entry value if the decrement time was reached. */
uint8_t scan_entry(struct entry *e) {
    if (minutes_diff(to_16bit_minutes(time(NULL)),e->decrtime)
        >= decr_every)
    {
        if (e->counter) {
            if (e->counter > LFU_INIT_VAL*2) {
                e->counter /= 2;
            } else {
                e->counter--;
            }
        }
        e->decrtime = to_16bit_minutes(time(NULL));
    }
    return e->counter;
}

/* Print the entry info. */
void show_entry(long pos, struct entry *e) {
    char *tag = "normal       ";

    if (pos >= 10 && pos <= 14) tag = "new no access";
    if (pos >= 15 && pos <= 19) tag = "new accessed ";
    if (pos >= keyspace_size -5) tag= "old no access";

    printf("%ld] <%s> frequency:%d decrtime:%d [%lu hits | age:%ld sec]\n",
        pos, tag, e->counter, e->decrtime, (unsigned long)e->hits,
            time(NULL) - e->ctime);
}

int main(void) {
    time_t start = time(NULL);
    time_t new_entry_time = start;
    time_t display_time = start;
    struct entry *entries = malloc(sizeof(*entries)*keyspace_size);
    long j;

    /* Initialize. */
    for (j = 0; j < keyspace_size; j++) {
        entries[j].counter = LFU_INIT_VAL;
        entries[j].decrtime = to_16bit_minutes(start);
        entries[j].hits = 0;
        entries[j].ctime = time(NULL);
    }

    while(1) {
        time_t now = time(NULL);
        long idx;

        /* Scan N random entries (simulates the eviction under maxmemory). */
        for (j = 0; j < 3; j++) {
            scan_entry(entries+(rand()%keyspace_size));
        }

        /* Access a random entry: use a power-law access pattern up to
         * 'switch_after' seconds. Then revert to flat access pattern. */
        if (now-start < switch_after) {
            /* Power law. */
            idx = 1;
            while((rand() % 21) != 0 && idx < keyspace_size) idx *= 2;
            if (idx > keyspace_size) idx = keyspace_size;
            idx = rand() % idx;
        } else {
            /* Flat. */
            idx = rand() % keyspace_size;
        }

        /* Never access entries between position 10 and 14, so that
         * we simulate what happens to new entries that are never
         * accessed VS new entries which are accessed in positions
         * 15-19.
         *
         * Also never access last 5 entry, so that we have keys which
         * are never recreated (old), and never accessed. */
        if ((idx < 10 || idx > 14) && (idx < keyspace_size-5))
            access_entry(entries+idx);

        /* Simulate the addition of new entries at positions between
         * 10 and 19, a random one every 10 seconds. */
        if (new_entry_time <= now) {
            idx = 10+(rand()%10);
            entries[idx].counter = LFU_INIT_VAL;
            entries[idx].decrtime = to_16bit_minutes(time(NULL));
            entries[idx].hits = 0;
            entries[idx].ctime = time(NULL);
            new_entry_time = now+10;
        }

        /* Show the first 20 entries and the last 20 entries. */
        if (display_time != now) {
            printf("=============================\n");
            printf("Current minutes time: %d\n", (int)to_16bit_minutes(now));
            printf("Access method: %s\n",
                (now-start < switch_after) ? "power-law" : "flat");

            for (j = 0; j < 20; j++)
                show_entry(j,entries+j);

            for (j = keyspace_size-20; j < keyspace_size; j++)
                show_entry(j,entries+j);
            display_time = now;
        }
    }
    return 0;
}

