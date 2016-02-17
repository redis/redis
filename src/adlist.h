/* adlist.h - A generic doubly linked list implementation
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

#ifndef __ADLIST_H__
#define __ADLIST_H__

/* Node, List, and Iterator are the only data structures used currently. */
/*
 * 列表节点结构体
 * 描述了列表中每一个节点的内容
 */
typedef struct listNode {
    //上一个节点的指针
    struct listNode *prev;
    //下一个节点的指针
    struct listNode *next;
    //节点值的指针
    void *value;
} listNode;

/*
 * 列表迭代器的结构体
 */
typedef struct listIter {
    //下一个节点的指针
    listNode *next;
    //迭代器指向
    int direction;
} listIter;

/*
 *列表的结构体
 */
typedef struct list {
    //表头
    listNode *head;
    //表尾
    listNode *tail;
    //复制回调函数指针
    void *(*dup)(void *ptr);
    //清空回调函数指针
    void (*free)(void *ptr);
    //比对回调函数指针
    int (*match)(void *ptr, void *key);
    //节点个数
    unsigned long len;
} list;

/* Functions implemented as macros */
//获取列表节点个数
#define listLength(l) ((l)->len)
//获取列表表头
#define listFirst(l) ((l)->head)
//获取列表表尾
#define listLast(l) ((l)->tail)
//获取前一个节点
#define listPrevNode(n) ((n)->prev)
//获取后一个节点
#define listNextNode(n) ((n)->next)
//获取节点值
#define listNodeValue(n) ((n)->value)
//设置列表的复制回调
#define listSetDupMethod(l,m) ((l)->dup = (m))
//设置列表的清空回调
#define listSetFreeMethod(l,m) ((l)->free = (m))
//设置列表的比较回调
#define listSetMatchMethod(l,m) ((l)->match = (m))
//获取列表的复制回调函数指针
#define listGetDupMethod(l) ((l)->dup)
//获取列表的清空回调函数指针
#define listGetFree(l) ((l)->free)
//获取列表的比较回调函数指针
#define listGetMatchMethod(l) ((l)->match)

/* Prototypes */
list *listCreate(void);
void listRelease(list *list);
list *listAddNodeHead(list *list, void *value);
list *listAddNodeTail(list *list, void *value);
list *listInsertNode(list *list, listNode *old_node, void *value, int after);
void listDelNode(list *list, listNode *node);
listIter *listGetIterator(list *list, int direction);
listNode *listNext(listIter *iter);
void listReleaseIterator(listIter *iter);
list *listDup(list *orig);
listNode *listSearchKey(list *list, void *key);
listNode *listIndex(list *list, long index);
void listRewind(list *list, listIter *li);
void listRewindTail(list *list, listIter *li);
void listRotate(list *list);

/* Directions for iterators */
/*迭代器方向*/
#define AL_START_HEAD 0 //从头指向为
#define AL_START_TAIL 1 //从尾指向头

#endif /* __ADLIST_H__ */
