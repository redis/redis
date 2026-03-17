#ifndef __FIFO_QUEUE_H
#define __FIFO_QUEUE_H

#include <stddef.h>

typedef struct fifoQueue fifoQueue;

fifoQueue *fifoQueueCreate(void);
void fifoQueueDestroy(fifoQueue *fq);
size_t fifoQueueSize(fifoQueue *fq);
void fifoQueueEnqueue(fifoQueue *fq, void *data);
void *fifoQueueDequeue(fifoQueue *fq);

#endif
