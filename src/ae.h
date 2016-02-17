/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __AE_H__
#define __AE_H__

//操作成功
#define AE_OK 0
//操作失败
#define AE_ERR -1

//事件未启用
#define AE_NONE 0
//写事件
#define AE_READABLE 1
//读事件
#define AE_WRITABLE 2
//处理文件事件
#define AE_FILE_EVENTS 1
//处理事件事件
#define AE_TIME_EVENTS 2
//全部事件
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
//当没有时间事件的时候事件池不要阻塞
#define AE_DONT_WAIT 4
//事件事件不需要进入下一个周期的标识
#define AE_NOMORE -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)
//事件池
struct aeEventLoop;

/* Types and data structures */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* File event structure */
//文件处理事件结构体
typedef struct aeFileEvent {
    //事件类型，设计原则是[AE_READABLE]和[AE_WRITABLE],但是代码中兼容他们的并集
    int mask; /* one of AE_(READABLE|WRITABLE) */
    //读文件回调
    aeFileProc *rfileProc;
    //写文件回调
    aeFileProc *wfileProc;
    //数据指针
    void *clientData;
} aeFileEvent;

/* Time event structure */
//时间事件结构体
typedef struct aeTimeEvent {
    //事件唯一ID
    long long id; /* time event identifier. */
    //到期的秒数
    long when_sec; /* seconds */
    //到期的毫秒数
    long when_ms; /* milliseconds */
    //到期回调
    aeTimeProc *timeProc;
    //删除时的回调
    aeEventFinalizerProc *finalizerProc;
    //数据
    void *clientData;
    //指向下一个时间事件
    struct aeTimeEvent *next;
} aeTimeEvent;

/* A fired event */
//唤醒事件的结构体
typedef struct aeFiredEvent {
    //事件索引
    int fd;
    //事件操作类型
    int mask;
} aeFiredEvent;

/* State of an event based program */
//事件池结构体
typedef struct aeEventLoop {
    //最大的文件处理事件索引
    int maxfd;   /* highest file descriptor currently registered */
    //文件事件容量
    int setsize; /* max number of file descriptors tracked */
    //下一个时间事件的ID
    long long timeEventNextId;
    //最后处理过事件的时间，主要用于事件系统事件被调回去了
    time_t lastTime;     /* Used to detect system clock skew */
    //文件处理事件缓冲区
    aeFileEvent *events; /* Registered events */
    //唤醒事件缓冲区
    aeFiredEvent *fired; /* Fired events */
    //事件事件列表头
    aeTimeEvent *timeEventHead;
    //处理循环退出标识
    int stop;
    //事件驱动的信息存储指针
    void *apidata; /* This is used for polling API specific data */
    //挂起前调用的回调函数指针
    aeBeforeSleepProc *beforesleep;
} aeEventLoop;

/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);

#endif
