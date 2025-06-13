/*
 * HNSW (Hierarchical Navigable Small World) Implementation
 * Based on the paper by Yu. A. Malkov, D. A. Yashunin
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 * Originally authored by: Salvatore Sanfilippo
 */

#define _DEFAULT_SOURCE
#define _USE_MATH_DEFINES
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>

#include "hnsw.h"

/* Get current time in milliseconds */
uint64_t ms_time(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}

/* Implementation of the recall test with random vectors. */
void test_recall(HNSW *index, int ef) {
    const int num_test_vectors = 10000;
    const int k = 100; // Number of nearest neighbors to find.
    if (ef < k) ef = k;

    // Add recall distribution counters (2% bins from 0-100%).
    int recall_bins[50] = {0};

    // Create array to store vectors for mixing.
    int num_source_vectors = 1000; // Enough, since we mix them.
    float **source_vectors = malloc(sizeof(float*) * num_source_vectors);
    if (!source_vectors) {
        printf("Failed to allocate memory for source vectors\n");
        return;
    }

    // Allocate memory for each source vector.
    for (int i = 0; i < num_source_vectors; i++) {
        source_vectors[i] = malloc(sizeof(float) * 300);
        if (!source_vectors[i]) {
            printf("Failed to allocate memory for source vector %d\n", i);
            // Clean up already allocated vectors.
            for (int j = 0; j < i; j++) free(source_vectors[j]);
            free(source_vectors);
            return;
        }
    }

    /* Populate source vectors from the index, we just scan the
     * first N items. */
    int source_count = 0;
    hnswNode *current = index->head;
    while (current && source_count < num_source_vectors) {
        hnsw_get_node_vector(index, current, source_vectors[source_count]);
        source_count++;
        current = current->next;
    }

    if (source_count < num_source_vectors) {
        printf("Warning: Only found %d nodes for source vectors\n",
            source_count);
        num_source_vectors = source_count;
    }

    // Allocate memory for test vector.
    float *test_vector = malloc(sizeof(float) * 300);
    if (!test_vector) {
        printf("Failed to allocate memory for test vector\n");
        for (int i = 0; i < num_source_vectors; i++) {
            free(source_vectors[i]);
        }
        free(source_vectors);
        return;
    }

    // Allocate memory for results.
    hnswNode **hnsw_results = malloc(sizeof(hnswNode*) * ef);
    hnswNode **linear_results = malloc(sizeof(hnswNode*) * ef);
    float *hnsw_distances = malloc(sizeof(float) * ef);
    float *linear_distances = malloc(sizeof(float) * ef);

    if (!hnsw_results || !linear_results || !hnsw_distances || !linear_distances) {
        printf("Failed to allocate memory for results\n");
        if (hnsw_results) free(hnsw_results);
        if (linear_results) free(linear_results);
        if (hnsw_distances) free(hnsw_distances);
        if (linear_distances) free(linear_distances);
        for (int i = 0; i < num_source_vectors; i++) free(source_vectors[i]);
        free(source_vectors);
        free(test_vector);
        return;
    }

    // Initialize random seed.
    srand(time(NULL));

    // Perform recall test.
    printf("\nPerforming recall test with EF=%d on %d random vectors...\n",
           ef, num_test_vectors);
    double total_recall = 0.0;

    for (int t = 0; t < num_test_vectors; t++) {
        // Create a random vector by mixing 3 existing vectors.
        float weights[3] = {0.0};
        int src_indices[3] = {0};

        // Generate random weights.
        float weight_sum = 0.0;
        for (int i = 0; i < 3; i++) {
            weights[i] = (float)rand() / RAND_MAX;
            weight_sum += weights[i];
            src_indices[i] = rand() % num_source_vectors;
        }

        // Normalize weights.
        for (int i = 0; i < 3; i++) weights[i] /= weight_sum;

        // Mix vectors.
        memset(test_vector, 0, sizeof(float) * 300);
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 300; j++) {
                test_vector[j] +=
                    weights[i] * source_vectors[src_indices[i]][j];
            }
        }

        // Perform HNSW search with the specified EF parameter.
        int slot = hnsw_acquire_read_slot(index);
        int hnsw_found = hnsw_search(index, test_vector, ef, hnsw_results, hnsw_distances, slot, 0);

        // Perform linear search (ground truth).
        int linear_found = hnsw_ground_truth_with_filter(index, test_vector, ef, linear_results, linear_distances, slot, 0, NULL, NULL);
        hnsw_release_read_slot(index, slot);

        // Calculate recall for this query (intersection size / k).
        if (hnsw_found > k) hnsw_found = k;
        if (linear_found > k) linear_found = k;
        int intersection_count = 0;
        for (int i = 0; i < linear_found; i++) {
            for (int j = 0; j < hnsw_found; j++) {
                if (linear_results[i] == hnsw_results[j]) {
                    intersection_count++;
                    break;
                }
            }
        }

        double recall = (double)intersection_count / linear_found;
        total_recall += recall;

        // Add to distribution bins (2% steps)
        int bin_index = (int)(recall * 50);
        if (bin_index >= 50) bin_index = 49; // Handle 100% recall case
        recall_bins[bin_index]++;

        // Show progress.
        if ((t+1) % 1000 == 0 || t == num_test_vectors-1) {
            printf("Processed %d/%d queries, current avg recall: %.2f%%\n",
                t+1, num_test_vectors, (total_recall / (t+1)) * 100);
        }
    }

    // Calculate and print final average recall.
    double avg_recall = (total_recall / num_test_vectors) * 100;
    printf("\nRecall Test Results:\n");
    printf("Average recall@%d (EF=%d): %.2f%%\n", k, ef, avg_recall);

    // Print recall distribution histogram.
    printf("\nRecall Distribution (2%% bins):\n");
    printf("================================\n");

    // Find the maximum bin count for scaling.
    int max_count = 0;
    for (int i = 0; i < 50; i++) {
        if (recall_bins[i] > max_count) max_count = recall_bins[i];
    }

    // Scale factor for histogram (max 50 chars wide)
    const int max_bars = 50;
    double scale = (max_count > max_bars) ? (double)max_bars / max_count : 1.0;

    // Print the histogram.
    for (int i = 0; i < 50; i++) {
        int bar_len = (int)(recall_bins[i] * scale);
        printf("%3d%%-%-3d%% | %-6d |", i*2, (i+1)*2, recall_bins[i]);
        for (int j = 0; j < bar_len; j++) printf("#");
        printf("\n");
    }

    // Cleanup.
    free(hnsw_results);
    free(linear_results);
    free(hnsw_distances);
    free(linear_distances);
    free(test_vector);
    for (int i = 0; i < num_source_vectors; i++) free(source_vectors[i]);
    free(source_vectors);
}

/* Example usage in main() */
int w2v_single_thread(int m_param, int quantization, uint64_t numele, int massdel, int self_recall, int recall_ef) {
    /* Create index */
    HNSW *index = hnsw_new(300, quantization, m_param);
    float v[300];
    uint16_t wlen;

    FILE *fp = fopen("word2vec.bin","rb");
    if (fp == NULL) {
        perror("word2vec.bin file missing");
        exit(1);
    }
    unsigned char header[8];
    if (fread(header,8,1,fp) <= 0) { // Skip header
        perror("Unexpected EOF");
        exit(1);
    }

    uint64_t id = 0;
    uint64_t start_time = ms_time();
    char *word = NULL;
    hnswNode *search_node = NULL;

    while(id < numele) {
        if (fread(&wlen,2,1,fp) == 0) break;
        word = malloc(wlen+1);
        if (fread(word,wlen,1,fp) <= 0) {
            perror("unexpected EOF");
            exit(1);
        }
        word[wlen] = 0;
        if (fread(v,300*sizeof(float),1,fp) <= 0) {
            perror("unexpected EOF");
            exit(1);
        }

        // Plain API that acquires a write lock for the whole time.
        hnswNode *added = hnsw_insert(index, v, NULL, 0, id++, word, 200);

        if (!strcmp(word,"banana")) search_node = added;
        if (!(id % 10000)) printf("%llu added\n", (unsigned long long)id);
    }
    uint64_t elapsed = ms_time() - start_time;
    fclose(fp);

    printf("%llu words added (%llu words/sec), last word: %s\n",
        (unsigned long long)index->node_count,
        (unsigned long long)id*1000/elapsed, word);

    /* Search query */
    if (search_node == NULL) search_node = index->head;
    hnsw_get_node_vector(index,search_node,v);
    hnswNode *neighbors[10];
    float distances[10];

    int found, j;
    start_time = ms_time();
    for (j = 0; j < 20000; j++)
        found = hnsw_search(index, v, 10, neighbors, distances, 0, 0);
    elapsed = ms_time() - start_time;
    printf("%d searches performed (%llu searches/sec), nodes found: %d\n",
        j, (unsigned long long)j*1000/elapsed, found);

    if (found > 0) {
        printf("Found %d neighbors:\n", found);
        for (int i = 0; i < found; i++) {
            printf("Node ID: %llu, distance: %f, word: %s\n",
                   (unsigned long long)neighbors[i]->id,
                   distances[i], (char*)neighbors[i]->value);
        }
    }

    // Self-recall test (ability to find the node by its own vector).
    if (self_recall) {
        hnsw_print_stats(index);
        hnsw_test_graph_recall(index,200,0);
    }

    // Recall test with random vectors.
    if (recall_ef > 0) {
        test_recall(index, recall_ef);
    }

    uint64_t connected_nodes;
    int reciprocal_links;
    hnsw_validate_graph(index, &connected_nodes, &reciprocal_links);

    if (massdel) {
        int remove_perc = 95;
        printf("\nRemoving %d%% of nodes...\n", remove_perc);
        uint64_t initial_nodes = index->node_count;

        hnswNode *current = index->head;
        while (current && index->node_count > initial_nodes*(100-remove_perc)/100) {
            hnswNode *next = current->next;
            hnsw_delete_node(index,current,free);
            current = next;
            // In order to don't remove only contiguous nodes, from time
            // skip a node.
            if (current && !(random() % remove_perc)) current = current->next;
        }
        printf("%llu nodes left\n", (unsigned long long)index->node_count);

        // Test again.
        hnsw_validate_graph(index, &connected_nodes, &reciprocal_links);
        hnsw_test_graph_recall(index,200,0);
    }

    hnsw_free(index,free);
    return 0;
}

struct threadContext {
    pthread_mutex_t FileAccessMutex;
    uint64_t numele;
    _Atomic uint64_t SearchesDone;
    _Atomic uint64_t id;
    FILE *fp;
    HNSW *index;
    float *search_vector;
};

// Note that in practical terms inserting with many concurrent threads
// may be *slower* and not faster, because there is a lot of
// contention. So this is more a robustness test than anything else.
//
// The optimistic commit API goal is actually to exploit the ability to
// add faster when there are many concurrent reads.
void *threaded_insert(void *ctxptr) {
    struct threadContext *ctx = ctxptr;
    char *word;
    float v[300];
    uint16_t wlen;

    while(1) {
        pthread_mutex_lock(&ctx->FileAccessMutex);
        if (fread(&wlen,2,1,ctx->fp) == 0) break;
        pthread_mutex_unlock(&ctx->FileAccessMutex);
        word = malloc(wlen+1);
        if (fread(word,wlen,1,ctx->fp) <= 0) {
            perror("Unexpected EOF");
            exit(1);
        }

        word[wlen] = 0;
        if (fread(v,300*sizeof(float),1,ctx->fp) <= 0) {
            perror("Unexpected EOF");
            exit(1);
        }

        // Check-and-set API that performs the costly scan for similar
        // nodes concurrently with other read threads, and finally
        // applies the check if the graph wasn't modified.
        InsertContext *ic;
        uint64_t next_id = ctx->id++;
        ic = hnsw_prepare_insert(ctx->index, v, NULL, 0, next_id, 200);
        if (hnsw_try_commit_insert(ctx->index, ic, word) == NULL) {
            // This time try locking since the start.
            hnsw_insert(ctx->index, v, NULL, 0, next_id, word, 200);
        }

        if (next_id >= ctx->numele) break;
        if (!((next_id+1) % 10000))
            printf("%llu added\n", (unsigned long long)next_id+1);
    }
    return NULL;
}

void *threaded_search(void *ctxptr) {
    struct threadContext *ctx = ctxptr;

    /* Search query */
    hnswNode *neighbors[10];
    float distances[10];
    int found = 0;
    uint64_t last_id = 0;

    while(ctx->id < 1000000) {
        int slot = hnsw_acquire_read_slot(ctx->index);
        found = hnsw_search(ctx->index, ctx->search_vector, 10, neighbors, distances, slot, 0);
        hnsw_release_read_slot(ctx->index,slot);
        last_id = ++ctx->id;
    }

    if (found > 0 && last_id == 1000000) {
        printf("Found %d neighbors:\n", found);
        for (int i = 0; i < found; i++) {
            printf("Node ID: %llu, distance: %f, word: %s\n",
                   (unsigned long long)neighbors[i]->id,
                   distances[i], (char*)neighbors[i]->value);
        }
    }
    return NULL;
}

int w2v_multi_thread(int m_param, int numthreads, int quantization, uint64_t numele) {
    /* Create index */
    struct threadContext ctx;

    ctx.index = hnsw_new(300, quantization, m_param);

    ctx.fp = fopen("word2vec.bin","rb");
    if (ctx.fp == NULL) {
        perror("word2vec.bin file missing");
        exit(1);
    }

    unsigned char header[8];
    if (fread(header,8,1,ctx.fp) <= 0) { // Skip header
        perror("Unexpected EOF");
        exit(1);
    }
    pthread_mutex_init(&ctx.FileAccessMutex,NULL);

    uint64_t start_time = ms_time();
    ctx.id = 0;
    ctx.numele = numele;
    pthread_t threads[numthreads];
    for (int j = 0; j < numthreads; j++)
        pthread_create(&threads[j], NULL, threaded_insert, &ctx);

    // Wait for all the threads to terminate adding items.
    for (int j = 0; j < numthreads; j++)
        pthread_join(threads[j],NULL);

    uint64_t elapsed = ms_time() - start_time;
    fclose(ctx.fp);

    // Obtain the last word.
    hnswNode *node = ctx.index->head;
    char *word = node->value;

    // We will search this last inserted word in the next test.
    // Let's save its embedding.
    ctx.search_vector = malloc(sizeof(float)*300);
    hnsw_get_node_vector(ctx.index,node,ctx.search_vector);

    printf("%llu words added (%llu words/sec), last word: %s\n",
        (unsigned long long)ctx.index->node_count,
        (unsigned long long)ctx.id*1000/elapsed, word);

    /* Search query */
    start_time = ms_time();
    ctx.id = 0; // We will use this atomic field to stop at N queries done.

    for (int j = 0; j < numthreads; j++)
        pthread_create(&threads[j], NULL, threaded_search, &ctx);

    // Wait for all the threads to terminate searching.
    for (int j = 0; j < numthreads; j++)
        pthread_join(threads[j],NULL);

    elapsed = ms_time() - start_time;
    printf("%llu searches performed (%llu searches/sec)\n",
        (unsigned long long)ctx.id,
        (unsigned long long)ctx.id*1000/elapsed);

    hnsw_print_stats(ctx.index);
    uint64_t connected_nodes;
    int reciprocal_links;
    hnsw_validate_graph(ctx.index, &connected_nodes, &reciprocal_links);
    printf("%llu connected nodes. Links all reciprocal: %d\n",
        (unsigned long long)connected_nodes, reciprocal_links);
    hnsw_free(ctx.index,free);
    return 0;
}

int main(int argc, char **argv) {
    int quantization = HNSW_QUANT_NONE;
    int numthreads = 0;
    uint64_t numele = 20000;
    int m_param = 0;  // Default value (0 means use HNSW_DEFAULT_M)

    /* This you can enable in single thread mode for testing: */
    int massdel = 0;       // If true, does the mass deletion test.
    int self_recall = 0;   // If true, does the self-recall test.
    int recall_ef = 0;     // If not 0, does the recall test with this EF value.

    for (int j = 1; j < argc; j++) {
        int moreargs = argc-j-1;

        if (!strcasecmp(argv[j],"--quant")) {
            quantization = HNSW_QUANT_Q8;
        } else if (!strcasecmp(argv[j],"--bin")) {
            quantization = HNSW_QUANT_BIN;
        } else if (!strcasecmp(argv[j],"--mass-del")) {
            massdel = 1;
        } else if (!strcasecmp(argv[j],"--self-recall")) {
            self_recall = 1;
        } else if (moreargs >= 1 && !strcasecmp(argv[j],"--recall")) {
            recall_ef = atoi(argv[j+1]);
            j++;
        } else if (moreargs >= 1 && !strcasecmp(argv[j],"--threads")) {
            numthreads = atoi(argv[j+1]);
            j++;
        } else if (moreargs >= 1 && !strcasecmp(argv[j],"--numele")) {
            numele = strtoll(argv[j+1],NULL,0);
            j++;
            if (numele < 1) numele = 1;
        } else if (moreargs >= 1 && !strcasecmp(argv[j],"--m")) {
            m_param = atoi(argv[j+1]);
            j++;
        } else if (!strcasecmp(argv[j],"--help")) {
            printf("%s [--quant] [--bin] [--thread <count>] [--numele <count>] [--m <count>] [--mass-del] [--self-recall] [--recall <ef>]\n", argv[0]);
            exit(0);
        } else {
            printf("Unrecognized option or wrong number of arguments: %s\n", argv[j]);
            exit(1);
        }
    }

    if (quantization == HNSW_QUANT_NONE) {
        printf("You can enable quantization with --quant\n");
    }

    if (numthreads > 0) {
        w2v_multi_thread(m_param, numthreads, quantization, numele);
    } else {
        printf("Single thread execution. Use --threads 4 for concurrent API\n");
        w2v_single_thread(m_param, quantization, numele, massdel, self_recall, recall_ef);
    }
}
