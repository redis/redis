#ifndef MEMORY_PREFETCH_H
#define MEMORY_PREFETCH_H

struct client;

void prefetchCommandsBatchInit(void);
int getConfigPrefetchBatchSize(void);
int addCommandToBatch(struct client *c);
void resetCommandsBatch(void);
void prefetchCommands(void);

#endif /* MEMORY_PREFETCH_H */
