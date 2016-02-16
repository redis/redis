/* adlist.c - A generic doubly linked list implementation
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
 * 这个模块实现了一个简单的列表功能
 * 使用双向表来实现相关功能
 */

#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/* Create a new list. The created list can be freed with
 * AlFreeList(), but private value of every node need to be freed
 * by the user before to call AlFreeList().
 *
 * On error, NULL is returned. Otherwise the pointer to the new list.
 *********************************************************************
 * 创建一个空的列表，如果创建不成功返回[NULL]，否则返回列表指针
 */
list *listCreate(void)
{
    //列表指针
    struct list *list;

    //申请内存空间
    if ((list = zmalloc(sizeof(*list))) == NULL)
        //申请不成功就返回null
        return NULL;
    //列表的首尾都指向[NULL]
    list->head = list->tail = NULL;
    //列表个数设置为[0]
    list->len = 0;
    //复制回调函数指针设置为[NULL]
    list->dup = NULL;
    //清空的回调函数指针设置为[NULL]
    list->free = NULL;
    //比对的回调函数指针设置为[NULL]
    list->match = NULL;
    //返回列表指针
    return list;
}

/* Free the whole list.
 *
 * This function can't fail.
 *********************************************************************
 * 释放列表，会释放所有的列表申请的内存，如果是这了清空的回调函数，还会为每一个
 * 列表节点调用清空函数
 * list   待释放的列表指针，不能为[NULL]
 */
void listRelease(list *list)
{
    unsigned long len;
    listNode *current, *next;
    //获取表头节点
    current = list->head;
    //获取列表节点个数
    len = list->len;
    //遍历列表的每一个节点
    while(len--) {
        //保存下一个节点
        next = current->next;
        //如果设置了清空的回调函数，就调用清空回调函数
        if (list->free) list->free(current->value);
        //释放节点内存
        zfree(current);
        //指向下一个节点
        current = next;
    }
    //释放列表内存
    zfree(list);
}

/* Add a new node to the list, to head, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned.
 *********************************************************************
 * 向列表头部添加节点，如果添加不成功返回[NULL],否则返回列表指针
 * 这个函数会同时修改双向表的节点关系
 * list   待添加的列表指针，不能为[NULL]
 * value  待添加的节点值
 */
list *listAddNodeHead(list *list, void *value)
{
    listNode *node;

    //为列表元素申请内存
    if ((node = zmalloc(sizeof(*node))) == NULL)
        //申请内存失败
        return NULL;
    //赋值
    node->value = value;
    if (list->len == 0) {//第一个节点
        //表头、表尾都需要指向第一个节点
        list->head = list->tail = node;
        //第一个元素的前一个、后一个节点都指向[NULL]
        node->prev = node->next = NULL;
    } else {//不是第一个节点
        //新元素的前一个节点指向[NULL]
        node->prev = NULL;
        //新节点的后一个节点指向原来的表头
        node->next = list->head;
        //原来表头的前一个节点指向新节点
        list->head->prev = node;
        //表头指向新节点
        list->head = node;
    }
    //节点个数加一
    list->len++;
    //返回列表指针
    return list;
}

/* Add a new node to the list, to tail, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned.
 *********************************************************************
 * 添加新节点到表尾，如果添加失败返回[NULL]，否则返回列表指针
 * 同时会设置新节点的节点关系
 * list   待添加的列表指针，不能为[NULL]
 * value  待添加的节点值
 */
list *listAddNodeTail(list *list, void *value)
{
    listNode *node;
    //为新节点申请内存空间
    if ((node = zmalloc(sizeof(*node))) == NULL)
        //内存生成失败，返回[NULL]
        return NULL;
    //赋值
    node->value = value;
    if (list->len == 0) {//第一个节点
        //列表的首尾都指向新节点
        list->head = list->tail = node;
        //新节点的前一个、后一个节点都指向[NULL]
        node->prev = node->next = NULL;
    } else {//不是第一个节点
        //新节点的前一个节点指向表尾
        node->prev = list->tail;
        //新节点的后一个节点指向[NULL]
        node->next = NULL;
        //原来表尾节点的下一个节点指向新节点
        list->tail->next = node;
        //表尾节点指向新节点
        list->tail = node;
    }
    //列表节点个数加一
    list->len++;
    //返回列表指针
    return list;
}

/*
 * 向列表中插入元素，插入失败返回[NULL],否则返回列表指针
 * 同时会设置好各个节点的关系
 * old_node必须是list的元素，为了效率这里代码中没有进行校验，所以调用的时候需要保证
 * list      待插入的列表指针，不能为[NULL]
 * old_node  参考节点，必须是list中的元素
 * value     待插入的节点值
 * after     相对参考节点的位置，非[0]时插入到参考节点后面，为[0]是插入到参考节点前面
 */
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    listNode *node;
    //申请新节点的内存
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    //赋值
    node->value = value;
    if (after) {//插入到参考节点后面
        //新节点的前一个节点指向参考节点
        node->prev = old_node;
        //新节点的后一个节点指向参考节点的后一个节点
        node->next = old_node->next;
        if (list->tail == old_node) {
            //如果参考节点是表为，则表尾指向新节点
            list->tail = node;
        }
    } else {//插入到参考节点之前
        //新节点的下一个节点指向参考节点
        node->next = old_node;
        //新节点的前一个节点指向参考节点的前一个节点
        node->prev = old_node->prev;
        if (list->head == old_node) {
            //如果参考节点是表头，则表头指向新节点
            list->head = node;
        }
    }
    if (node->prev != NULL) {
        //如果新节点的前一个节点不为[NULL]
        //则新节点的上一个节点的下一个节点指向新节点
        node->prev->next = node;
    }
    if (node->next != NULL) {
        //如果新节点的下一个节点不为[NULL]
        //则新节点的下一个节点的前一个节点指向新节点
        node->next->prev = node;
    }
    //列表节点个数加一
    list->len++;
    //返回列表指针
    return list;
}

/* Remove the specified node from the specified list.
 * It's up to the caller to free the private value of the node.
 *
 * This function can't fail.
 *********************************************************************
 * 从列表中删除节点
 * node必须是列表中的元素，否则会有问题
 * 为了提高效率，代码中没有进行校验，所以调用的时候需要保证
 * list   待处理的列表指针，不能为[NULL]
 * node   待删除的节点指针，必须在list中
 */
void listDelNode(list *list, listNode *node)
{
    if (node->prev)
        //如果待删除的节点前一个节点不为[NULL]
        //就把前一个节点的下一个节点指向待删除的节点的下一个节点
        node->prev->next = node->next;
    else
        //否则表头指向待删除的节点的下一个节点
        //[list->head]和[node-prev==NULL]的节点是一致的
        list->head = node->next;
    if (node->next)
        //如果待删除的节点的下一个节点不为[NULL]
        //就把下一个节点的上一个节点指向待删除节点的上一个节点
        node->next->prev = node->prev;
    else
        //否则表尾指向待删除的节点的上一个节点
        //[list->tail]和[node-next==NULL]的节点是一致的
        list->tail = node->prev;
    //如果设置了清空回调函数，就调用清空回调函数
    if (list->free) list->free(node->value);
    //是否节点内存
    zfree(node);
    //列表节点个数减一
    list->len--;
}

/* Returns a list iterator 'iter'. After the initialization every
 * call to listNext() will return the next element of the list.
 *
 * This function can't fail.
 *********************************************************************
 * 获取列表的迭代器，如果获取失败就返回[NULL]
 * list       待处理的列表指针，不能为[NULL]
 * direction  迭代器迭代方向
 *            [AL_START_HEAD]=[0]从头指向尾
 *            [AL_START_TAIL]=[1]从尾指向头
 */
listIter *listGetIterator(list *list, int direction)
{
    listIter *iter;
    //为迭代器申请内存，如果申请失败返回[NULL]
    if ((iter = zmalloc(sizeof(*iter))) == NULL) return NULL;
    if (direction == AL_START_HEAD)
        //从头开始的情况，迭代器就指向表头
        iter->next = list->head;
    else
        //从尾开始的情况，迭代器就指向表尾
        iter->next = list->tail;
    //设置迭代器指向
    iter->direction = direction;
    //返回迭代器指针
    return iter;
}

/* Release the iterator memory
 *********************************************************************
 * 释放迭代器内存
 * iter   待释放的迭代器指针
 */
void listReleaseIterator(listIter *iter) {
    //直接释放内存，不用做其它处理
    zfree(iter);
}

/* Create an iterator in the list private iterator structure
 *********************************************************************
 * 迭代器定位到表头
 * list   待指向的列表，不能为[NULL]
 * li     需要重新定位的迭代器指针
 */
void listRewind(list *list, listIter *li) {
    //指向表头
    li->next = list->head;
    //设置迭代器指向为从头开始
    li->direction = AL_START_HEAD;
}

/*
 * 迭代器定位到表尾
 * list   待指向的列表，不能为[NULL]
 * li     需要重新定位的迭代器指针
 */
void listRewindTail(list *list, listIter *li) {
    //指向表尾
    li->next = list->tail;
    //设置迭代器指向为从尾开始
    li->direction = AL_START_TAIL;
}

/* Return the next element of an iterator.
 * It's valid to remove the currently returned element using
 * listDelNode(), but not to remove other elements.
 *
 * The function returns a pointer to the next element of the list,
 * or NULL if there are no more elements, so the classical usage patter
 * is:
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }
 *********************************************************************
 * 根据迭代器获取下一个节点
 * iter 迭代器指针
 */
listNode *listNext(listIter *iter)
{
    //获取迭代器指向的节点指针
    listNode *current = iter->next;

    if (current != NULL) {
        //如果当前节点不为[NULL]，就需要设置迭代器指向新的节点
        if (iter->direction == AL_START_HEAD)
            //从头开始的情况，迭代器就指向当前节点的下一个节点
            iter->next = current->next;
        else
            //从尾开始的情况，迭代器就指向当前节点的上一个节点
            iter->next = current->prev;
    }
    //返回当前节点
    return current;
}

/* Duplicate the whole list. On out of memory NULL is returned.
 * On success a copy of the original list is returned.
 *
 * The 'Dup' method set with listSetDupMethod() function is used
 * to copy the node value. Otherwise the same pointer value of
 * the original node is used as value of the copied node.
 *
 * The original list both on success or error is never modified.
 *********************************************************************
 * 复制列表，会复制列表中的所有节点，并保持节点的顺序
 * 如果设置了复制回调函数，还会为每一个节点调用复制函数
 * 复制成功返回新列表的指针，否则返回[NULL]
 * orig   作为数据源的列表，不能为[NULL]
 */
list *listDup(list *orig)
{
    list *copy;
    listIter *iter;
    listNode *node;
    //创建一个空里列表
    if ((copy = listCreate()) == NULL)
        //创建不成功，直接返回[NULL]
        return NULL;
    //设置复制回调函数指针
    copy->dup = orig->dup;
    //设置清空回调函数指针
    copy->free = orig->free;
    //设置比对回到函数指针
    copy->match = orig->match;
    //获取从头开始的迭代器
    iter = listGetIterator(orig, AL_START_HEAD);
    //遍历全部的元素，并进行复制
    while((node = listNext(iter)) != NULL) {
        void *value;
        //如果设置了复制回调函数指针，就调用回调函数来获取节点值
        //否则直接把源节点的值复制过来
        if (copy->dup) {
            //调用复制回调函数
            //这是深拷贝
            value = copy->dup(node->value);
            if (value == NULL) {
                //复制回调函数调用失败
                //释放新的列表
                listRelease(copy);
                //释放迭代器
                listReleaseIterator(iter);
                return NULL;
            }
        } else
            //直接复制，这是浅拷贝
            value = node->value;
        //把复制得到的节点添加到新列表的末尾
        if (listAddNodeTail(copy, value) == NULL) {
            //添加失败，释放资源，并返回[NULL]
            listRelease(copy);
            listReleaseIterator(iter);
            return NULL;
        }
    }
    //释放迭代器
    listReleaseIterator(iter);
    //返回新列表的指针
    return copy;
}

/* Search the list for a node matching a given key.
 * The match is performed using the 'match' method
 * set with listSetMatchMethod(). If no 'match' method
 * is set, the 'value' pointer of every node is directly
 * compared with the 'key' pointer.
 *
 * On success the first matching node pointer is returned
 * (search starts from head). If no matching node exists
 * NULL is returned.
 *********************************************************************
 * 从列表中查找第一个值为key的节点
 * 从头开始查找，找到第一个符合的节点就返回
 * list   待查找的列表
 * key    比对的节点值
 */
listNode *listSearchKey(list *list, void *key)
{
    listIter *iter;
    listNode *node;
    //获取一个从头开始的迭代器
    iter = listGetIterator(list, AL_START_HEAD);
    //遍历节点，对每一个节点进行比较
    while((node = listNext(iter)) != NULL) {
        if (list->match) {
            //设置了比较回调函数指针，就用回调函数进行比较
            if (list->match(node->value, key)) {
                //如果一样就释放迭代器，并返回节点指针
                listReleaseIterator(iter);
                return node;
            }
        } else {
            //否则直接比较key和节点的value是不是一样的
            if (key == node->value) {
                //如果一样就是否迭代器，并返回节点指针
                listReleaseIterator(iter);
                return node;
            }
        }
    }
    //没有找到满足要求的节点，释放迭代器，并返回[NULL]
    listReleaseIterator(iter);
    return NULL;
}

/* Return the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range NULL is returned.
 *********************************************************************
 * 获取列表中指定位置的节点
 * list    待查找的列表
 * index   >=0时从头开始查找，<=-1时从尾开始查找，如果超出范围就返回[NULL]
 */
listNode *listIndex(list *list, long index) {
    listNode *n;

    if (index < 0) {
        //从未开始查找
        //倒数第一个为[-1]，倒数第二个为[-2]...
        index = (-index)-1;
        //先指向表尾
        n = list->tail;
        //循环遍历，到达位置后退出
        while(index-- && n) n = n->prev;
    } else {
        //指向表头
        n = list->head;
        //循环遍历，到达位置后退出
        while(index-- && n) n = n->next;
    }
    //返回找到的节点指针，如果没有找到则是[NULL]
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head.
 *********************************************************************
 * 把列表的表尾移到表头去
 * list 待处理的列表
 */
void listRotate(list *list) {
    //获取表尾
    listNode *tail = list->tail;
    //如果只有一个节点元素，就不用处理了
    if (listLength(list) <= 1) return;

    /* Detach current tail */
    //表尾指向原来的表尾的前一个
    list->tail = tail->prev;
    //新表尾的下一个指向[NULL]
    list->tail->next = NULL;
    /* Move it as head */
    //表头的上一个指向原来的表尾
    list->head->prev = tail;
    //原来的表尾的上一个指向[NULL]
    tail->prev = NULL;
    //原来表尾的下一个指向表头
    tail->next = list->head;
    //表头更新为原来的表尾
    list->head = tail;
}
