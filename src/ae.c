/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * 这个模块实现了事件处理，封装了基本的事件库
 * 事件处理库的使用优先为：EVPORT > EPOLL > KQUEUE > SELECT
 */
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "ae.h"
#include "zmalloc.h"
#include "config.h"

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */

/*
 * 通过宏来使用不同的事件驱动，所以事件驱动的选择是在编译时期确定的
 * 运行时期是不能更改的
 */
#ifdef HAVE_EVPORT
#include "ae_evport.c"
#else
    #ifdef HAVE_EPOLL
    #include "ae_epoll.c"
    #else
        #ifdef HAVE_KQUEUE
        #include "ae_kqueue.c"
        #else
        #include "ae_select.c"
        #endif
    #endif
#endif

/*
 * 创建一个事件池，用于管理事件
 * setsize   事件池中事件缓冲区的大小
 */
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;
    //分配内存
    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;
    //分配事件缓冲区的内存
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*setsize);
    //分配需要处理的事件缓冲区
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*setsize);
    //如果缓冲区内存分配失败，就直接跳到错误处理的地方
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    //设置缓冲区大小
    eventLoop->setsize = setsize;
    //记录事件
    eventLoop->lastTime = time(NULL);
    //时间事件起始设置为[NULL]
    eventLoop->timeEventHead = NULL;
    //事件时间事件的ID初始设置为[0]
    eventLoop->timeEventNextId = 0;
    //事件池标记为不停止
    eventLoop->stop = 0;
    //最大的句柄
    eventLoop->maxfd = -1;
    //每次事件等待之前调用的回调函数设置为[NULL]
    eventLoop->beforesleep = NULL;
    //调用具体事件处理驱动中的函数来启动事件驱动
    if (aeApiCreate(eventLoop) == -1) goto err;
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
    //把每一个事件都标记为[AE_NONE]
    for (i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;

    //返回事件池指针
    return eventLoop;

err:
    //错误处理
    if (eventLoop) {//如果已经申请了内存就需要释放内存
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

/* Return the current set size. */
/*
 * 获取事件池的容量
 * eventLoop    需要访问的事件池指针
 */
int aeGetSetSize(aeEventLoop *eventLoop) {
    //直接返回大小
    return eventLoop->setsize;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful. */
/*
 * 重新设置事件池的容量，操作成功返回[AE_OK]=[0],否则返回[AE_ERR]=[-1]
 * eventLoop   待处理的事件池指针
 * setsize     更新后的大小
 */
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    int i;
    //如果新容量等于老的容量就直接返回
    if (setsize == eventLoop->setsize) return AE_OK;
    //如果新的容量小于事件池中已使用的容量大小，返回错误
    if (eventLoop->maxfd >= setsize) return AE_ERR;
    //调用事件驱动力的函数进行处理，如果错误就直接返回错误
    if (aeApiResize(eventLoop,setsize) == -1) return AE_ERR;
    //调整事件缓冲区的大小
    eventLoop->events = zrealloc(eventLoop->events,sizeof(aeFileEvent)*setsize);
    //调整执行中的事件句柄缓冲区大小
    eventLoop->fired = zrealloc(eventLoop->fired,sizeof(aeFiredEvent)*setsize);
    //设置容量大小
    eventLoop->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an AE_NONE mask. */
    //设置未使用的缓冲区为[AE_NONE]
    for (i = eventLoop->maxfd+1; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return AE_OK;
}

/*
 * 删除事件池，同时会删除事件驱动中的内容，并释放内存
 * eventLoop   待删除的事件池
 */
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    //删除事件驱动中的内容
    aeApiFree(eventLoop);
    //释放内存
    zfree(eventLoop->events);
    zfree(eventLoop->fired);
    zfree(eventLoop);
}

/*
 * 停止事件池的事件处理，本质就是标记为停止
 * eventLoop 待处理的事件池指针
 */
void aeStop(aeEventLoop *eventLoop) {
    //直接标记为停止
    eventLoop->stop = 1;
}

/*
 * 创建一个文件处理事件，进行文件的读写操作
 * eventLoop    事件池指针
 * fd           事件索引
 * mask         事件类型，这里支持[AE_READABLE]和[AE_WRITABLE]
 * proc         文件事件的处理回调
 * clientData   数据内容指针
 */
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData)
{
    //如果事件索引操作了池子的容量返回错误
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }
    //获取事件指针
    aeFileEvent *fe = &eventLoop->events[fd];
    //通过事件驱动来添加事件
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        //添加失败直接返回
        return AE_ERR;
    //更新事件标识
    fe->mask |= mask;
    //设置文件事件的处理函数回调
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    //设置数据
    fe->clientData = clientData;
    //如果当前事件的索引大于之前已启用的事件最大索引，就更新最大索引
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}

/*
 * 删除文件处理事件
 * eventLoop    事件池指针
 * fd           事件索引
 * mask         需要删除的事件类型
 */
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    //如果超出容量就直接返回
    if (fd >= eventLoop->setsize) return;
    //获取事件指针
    aeFileEvent *fe = &eventLoop->events[fd];
    //如果事件本来就没有启用，直接返回
    if (fe->mask == AE_NONE) return;
    //从事件驱动中删除对应的事件
    aeApiDelEvent(eventLoop, fd, mask);
    //修改事件类型
    fe->mask = fe->mask & (~mask);
    //如果被删除的事件是事件池中的最大启用事件，还需要调整最大启用事件的索引
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;
        //遍历找到最大的一个启动事件的索引
        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }
}

/*
 * 通过事件索引查找事件类型
 * eventLoop   事件池指针
 * fd          事件索引
 */
int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    //超出容量，直接返回
    if (fd >= eventLoop->setsize) return 0;
    //获取事件指针
    aeFileEvent *fe = &eventLoop->events[fd];
    //返回类型
    return fe->mask;
}

/*
 * 获取当前时间
 * seconds      存放秒数的内存地址
 * milliseconds 存放毫秒数的内存地址
 */
static void aeGetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;
    //获取当前时间
    gettimeofday(&tv, NULL);
    //获取秒数
    *seconds = tv.tv_sec;
    //获取毫秒数
    *milliseconds = tv.tv_usec/1000;
}

/*
 *在当前时间的基础上推迟相应的毫秒
 * milliseconds   需要推迟的毫秒数
 * sec            存放得到的时间的秒数的内存地址
 * ms             存放得到的事件的毫秒数的内存地址
 */
static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;
    //获取当前时间的秒数和毫秒数
    aeGetTime(&cur_sec, &cur_ms);
    //计算新的秒数
    when_sec = cur_sec + milliseconds/1000;
    //计算新的毫秒数
    when_ms = cur_ms + milliseconds%1000;
    if (when_ms >= 1000) {
        //如果新的毫秒数大于1000，需要秒数进一
        when_sec ++;
        when_ms -= 1000;
    }
    //赋值
    *sec = when_sec;
    *ms = when_ms;
}

/*
 * 创建一个定时事件
 * eventLoop        事件池指针
 * milliseconds     需要过多长事件被执行
 * proc             事件到达是调用的回调函数指针
 * clientData       数据
 * finalizerProc    时间事件被删除的时候调用的回调函数指针
 */
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc)
{
    //时间事件的ID在原来的ID上加一得到
    long long id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;
    //申请内存空间
    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    //初始化各种值
    te->id = id;
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    //添加到事件事件列表的表头
    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;
    //返回id
    return id;
}


/*
 * 删除指定id的时间事件
 * eventLoop    事件池指针
 * id           时间事件的唯一标识符
 */
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id)
{
    aeTimeEvent *te, *prev = NULL;
    //获取表头
    te = eventLoop->timeEventHead;
    //循环遍历，查找id匹配的事件
    while(te) {
        if (te->id == id) {
            //如果是表头，需要把事件池的时间事件表头指向下一个
            if (prev == NULL)
                eventLoop->timeEventHead = te->next;
            else
                //否则直接跳过这个就可以了
                prev->next = te->next;
            //如果设置了退出回调函数指针，就调用这个回调函数
            if (te->finalizerProc)
                te->finalizerProc(eventLoop, te->clientData);
            //是否内存
            zfree(te);
            return AE_OK;
        }
        //指向下一个
        prev = te;
        te = te->next;
    }
    //没有匹配的时间事件，返回错误
    return AE_ERR; /* NO event with the specified ID found */
}

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
/*
 * 查找离触发事件最近的一个时间事件
 * eventLoop    需要查找的事件池指针
 */
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop)
{
    //获取第一个时间事件的指针
    aeTimeEvent *te = eventLoop->timeEventHead;
    //存放查找到的最近的时间事件
    aeTimeEvent *nearest = NULL;

    //遍历全部的时间事件
    while(te) {
        if (!nearest || te->when_sec < nearest->when_sec ||
                (te->when_sec == nearest->when_sec &&
                 te->when_ms < nearest->when_ms))
            //找到更近的事件
            nearest = te;
        te = te->next;
    }
    //返回找到的最近的时间事件指针
    return nearest;
}

/* Process time events */
/*
 * 处理时间事件
 * eventLoop    需要处理的事件池
 */
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;
    //当前事件
    time_t now = time(NULL);

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */
    //这个地方是为了适应系统事件被调整导致的事件错乱的问题的
    //如果出现了这种情况，就让所有的事件都会被立即执行
    //redis认为提前处理的危害小于延迟处理
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while(te) {
            //秒数设置为[0]，那么事件会被接下来立即执行
            te->when_sec = 0;
            te = te->next;
        }
    }
    //设置最后的执行事件
    eventLoop->lastTime = now;

    //获取表头
    te = eventLoop->timeEventHead;
    //最大id值
    maxId = eventLoop->timeEventNextId-1;
    while(te) {
        long now_sec, now_ms;
        long long id;
        //id不合法，跳过吧，这种情况应该是不正常的
        //对应的事件会在将来被执行，就是id自然增加到一定值的时候
        if (te->id > maxId) {
            te = te->next;
            continue;
        }
        //获取当前事件
        aeGetTime(&now_sec, &now_ms);
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            //时间事件到时了，需要处理啦~~~~~~~~
            int retval;

            id = te->id;
            //调用事件处理回调函数
            retval = te->timeProc(eventLoop, id, te->clientData);
            //计数
            processed++;
            /* After an event is processed our time event list may
             * no longer be the same, so we restart from head.
             * Still we make sure to don't process events registered
             * by event handlers itself in order to don't loop forever.
             * To do so we saved the max ID we want to handle.
             *
             * FUTURE OPTIMIZATIONS:
             * Note that this is NOT great algorithmically. Redis uses
             * a single time event so it's not a problem but the right
             * way to do this is to add the new elements on head, and
             * to flag deleted elements in a special way for later
             * deletion (putting references to the nodes to delete into
             * another linked list). */
            //如果时间处理函数返回一个不是[-1]的值，表示需要进入下一个等待
            //这里需要注意返回的值不能小于[-1]，否则就会被再次立即执行，这样是不合理的
            if (retval != AE_NOMORE) {
                aeAddMillisecondsToNow(retval,&te->when_sec,&te->when_ms);
            } else {
                //否则就删除事件
                aeDeleteTimeEvent(eventLoop, id);
            }
            //处理完一个后需要从表头重新遍历
            te = eventLoop->timeEventHead;
        } else {
            te = te->next;
        }
    }
    //返回被执行的次数
    return processed;
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * the events that's possible to process without to wait are processed.
 *
 * The function returns the number of events processed. */
/*
 * 事件处理函数
 * eventLoop    事件池指针
 * flags        事件处理标识
 */
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;
    //如果没有设置时间或文件事件的标识，就直接返回[0]
    /* Nothing to do? return ASAP */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
    if (eventLoop->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        //走到这里表明需要使用事件驱动的唤醒功能
        int j;
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;
        //如果需要处理时间事件并且没有设置[AE_DONT_WAIT],就需要查找到最近的一个时间事件
        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            shortest = aeSearchNearestTimer(eventLoop);
        if (shortest) {
            //找到了最近的一个时间事件
            long now_sec, now_ms;

            /* Calculate the time missing for the nearest
             * timer to fire. */
            aeGetTime(&now_sec, &now_ms);
            //计算这个事件的到期事件
            tvp = &tv;
            tvp->tv_sec = shortest->when_sec - now_sec;
            if (shortest->when_ms < now_ms) {
                tvp->tv_usec = ((shortest->when_ms+1000) - now_ms)*1000;
                tvp->tv_sec --;
            } else {
                tvp->tv_usec = (shortest->when_ms - now_ms)*1000;
            }
            if (tvp->tv_sec < 0) tvp->tv_sec = 0;
            if (tvp->tv_usec < 0) tvp->tv_usec = 0;
        } else {
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout
             * to zero */
             //如果是不等待，就直接设置唤醒事件为0
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                //否则就一直等到需要处理事件
                //到这里就会使事件驱动阻塞了
                /* Otherwise we can block */
                tvp = NULL; /* wait forever */
            }
        }
        //调用事件驱动的函数，等待唤醒或超时
        numevents = aeApiPoll(eventLoop, tvp);
        //处理需要处理的全部文件事件
        for (j = 0; j < numevents; j++) {
            //得到唤醒的事件指针
            aeFileEvent *fe = &eventLoop->events[eventLoop->fired[j].fd];
            int mask = eventLoop->fired[j].mask;
            int fd = eventLoop->fired[j].fd;
            int rfired = 0;

	    /* note the fe->mask & mask & ... code: maybe an already processed
             * event removed an element that fired and we still didn't
             * processed, so we check if the event is still valid. */
            if (fe->mask & mask & AE_READABLE) {
                //读炒作
                rfired = 1;
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
            }
            if (fe->mask & mask & AE_WRITABLE) {
                //读写回调一致，并且刚刚调用过读回调，就不需要再调用了
                //否则还需要调用写回调
                if (!rfired || fe->wfileProc != fe->rfileProc)
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
            }
            //事件处理计数
            processed++;
        }
    }
    /* Check time events */
    //处理时间事件
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    //返回全部处理过的事件个数
    return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
/*
 * 等待指定毫秒，直到对应的事件可读，科协或可运行
 * 这个函数直接使用的poll函数，可能会有问题，具体还不清楚
 * fd           检测的句柄
 * mask         检测的事件
 * milliseconds 超时事件
 */
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;
    //初始化
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    //初始化状态
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;
    //等待
    if ((retval = poll(&pfd, 1, milliseconds))== 1) {
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
	    if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}

/*
 * 事件池处理的入口函数
 * eventLoop        需要处理的事件池
 */
void aeMain(aeEventLoop *eventLoop) {
    //标记为运行
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
        //一直循环执行，知道标记为停止
        if (eventLoop->beforesleep != NULL)
            //进入事件驱动等待前调用一个回调
            eventLoop->beforesleep(eventLoop);
        //事件处理函数，如果没有时间事件，它会阻塞
        aeProcessEvents(eventLoop, AE_ALL_EVENTS);
    }
}

/*
 * 获取具体的事件驱动名称
 */
char *aeGetApiName(void) {
    return aeApiName();
}

/*
 * 设置进入事件驱动等待前的回调函数指针
 */
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}
