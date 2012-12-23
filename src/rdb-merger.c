#include "redis.h"
#include "gd.h"
#include <unistd.h>
#include <stdio.h>

/* Prototypes taken from redis.c (missing in redis.h) */
void initServerConfig();
void createSharedObjects(void);

/* Stuff from initServer() that we need for rdb operations to work */
void init() {
    setupSignalHandlers(); /* Mainly so we get stack traces */
    createSharedObjects();
    server.loading_process_events_interval_bytes = 0; /* Make sure we don't attempt to process events while loading (because there's no event loop) */
    if (!server.logfile) {
        server.logfile = "/dev/stderr"; /* Force logs to go to stderr since stdout is (optionally) used for the conversion output */
    }
}

static void printUsage() {
    fprintf(stderr, "Garantia RDB merger\n");
    fprintf(stderr, "  rdb-merger [-h][-c redis_conf.conf][-p] -o output_file.rdb input_file.rdb [input_file.rdb ...]\n");
    fprintf(stderr, "  Special output file name \"-\" will output to stdout.\n");
}

int main(int argc, char **argv) {
    int opt;
    char *conf = NULL;
    char *outfile = NULL;
    int progress = 0;
    /* Parse args */
    while ((opt = getopt(argc, argv, "hc:o:p")) != -1) {
        switch (opt) {
            case 'c':
                conf = optarg;
                break;
            case 'o':
                outfile = optarg;
                break;
            case 'p':
                progress = 1;
                break;
            case 'h':
                printUsage();
                return 1;
        }
    }
    
    if (!outfile) {
        fprintf(stderr, "No output file specified, try 'rdb-merger -h' for help.\n");
        return 1;
    }
    
    if (optind >= argc) {
        fprintf(stderr, "Missing input file[s], try 'rdb-merger -h' for help.\n");
        return 1;
    }
    
    
    initServerConfig();

    if (conf) {
        resetServerSaveParams();
        loadServerConfig(conf);
    }
    
    init();
    
    if (mergerRdbs(argc - optind, argv + optind, outfile, progress) != REDIS_OK) {
        fprintf(stderr, "Error merging rdb files\n");
        return 1;
    }
    
    return 0;
}

