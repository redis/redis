/* Linux epoll(2) based ae.c module
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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
 * 封装的使用epoll进行事件驱动，对外实现了一些统一的接口
 */

#include <sys/epoll.h>

//epoll事件驱动的信息结构体
typedef struct aeApiState {
    //事件驱动句柄
    int epfd;
    //事件缓冲区
    struct epoll_event *events;
} aeApiState;

/*
 * 创建基于epoll的事件驱动
 * eventLoop    对应的事件池指针
 */
static int aeApiCreate(aeEventLoop *eventLoop) {
    //申请内存空间
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    //新建事件缓冲区
    state->events = zmalloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    //在事件驱动epoll中创建
    state->epfd = epoll_create(1024); /* 1024 is just a hint for the kernel */
    if (state->epfd == -1) {
        //创建失败，释放资源
        zfree(state->events);
        zfree(state);
        return -1;
    }
    //创建成功
    eventLoop->apidata = state;
    return 0;
}

/*
 * 调整事件容量
 * setsize  新的事件容量
 */
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;
    //重新分配内存
    state->events = zrealloc(state->events, sizeof(struct epoll_event)*setsize);
    return 0;
}

/*
 * 关闭事件，释放内存
 * eventLoop    关联的事件池指针
 */
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;
    //关闭句柄
    close(state->epfd);
    //清空内存
    zfree(state->events);
    zfree(state);
}

/*
 * 添加事件
 * eventLoop    关联的事件池指针
 * fd           对应的事件索引
 * mask         事件类型
 */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee;
    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation. */
    //如果原来是未启用的就是添加，否则就是修改
    int op = eventLoop->events[fd].mask == AE_NONE ?
            EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    ee.events = 0;
    //计算新的类型
    mask |= eventLoop->events[fd].mask; /* Merge old events */
    //计算epoll识别的事件类型
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.u64 = 0; /* avoid valgrind warning */
    ee.data.fd = fd;
    //执行操作
    if (epoll_ctl(state->epfd,op,fd,&ee) == -1) return -1;
    return 0;
}

/*
 * 删除事件
 * eventLoop    关联的事件池指针
 * fd           事件索引
 * delmask      待删除的事件类型
 */
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee;
    //计算新的事件类型
    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    //计算epoll可识别的事件类型
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.u64 = 0; /* avoid valgrind warning */
    ee.data.fd = fd;
    if (mask != AE_NONE) {
        //新的事件类型不是为未启用，就是修改
        epoll_ctl(state->epfd,EPOLL_CTL_MOD,fd,&ee);
    } else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
         //否则就是删除
        epoll_ctl(state->epfd,EPOLL_CTL_DEL,fd,&ee);
    }
}

/*
 * 事件等待函数
 * eventLoop    关联的事件池指针
 * timeval      超时时间
 */
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;
    //等待，直到超时
    retval = epoll_wait(state->epfd,state->events,eventLoop->setsize,
            tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);
    if (retval > 0) {
        //不是超时退出的
        int j;
        //遍历全部可处理事件
        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events+j;
            //计算可处理的事件类型
            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & EPOLLERR) mask |= AE_WRITABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE;
            //记录可处理的事件信息
            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    }
    //返回可处理的事件个数
    return numevents;
}

/*
 *获取事件驱动的名称
 */
static char *aeApiName(void) {
    return "epoll";
}
