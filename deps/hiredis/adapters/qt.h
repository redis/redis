/*-
 * Copyright (C) 2014 Pietro Cerutti <gahr@gahr.ch>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef __HIREDIS_QT_H__
#define __HIREDIS_QT_H__
#include <QSocketNotifier>
#include "../async.h"

static void RedisQtAddRead(void *);
static void RedisQtDelRead(void *);
static void RedisQtAddWrite(void *);
static void RedisQtDelWrite(void *);
static void RedisQtCleanup(void *);

class RedisQtAdapter : public QObject {

    Q_OBJECT

    friend
    void RedisQtAddRead(void * adapter) {
        RedisQtAdapter * a = static_cast<RedisQtAdapter *>(adapter);
        a->addRead();
    }

    friend
    void RedisQtDelRead(void * adapter) {
        RedisQtAdapter * a = static_cast<RedisQtAdapter *>(adapter);
        a->delRead();
    }

    friend
    void RedisQtAddWrite(void * adapter) {
        RedisQtAdapter * a = static_cast<RedisQtAdapter *>(adapter);
        a->addWrite();
    }

    friend
    void RedisQtDelWrite(void * adapter) {
        RedisQtAdapter * a = static_cast<RedisQtAdapter *>(adapter);
        a->delWrite();
    }

    friend
    void RedisQtCleanup(void * adapter) {
        RedisQtAdapter * a = static_cast<RedisQtAdapter *>(adapter);
        a->cleanup();
    }

    public:
        RedisQtAdapter(QObject * parent = 0)
            : QObject(parent), m_ctx(0), m_read(0), m_write(0) { }

        ~RedisQtAdapter() {
            if (m_ctx != 0) {
                m_ctx->ev.data = NULL;
            }
        }

        int setContext(redisAsyncContext * ac) {
            if (ac->ev.data != NULL) {
                return REDIS_ERR;
            }
            m_ctx = ac;
            m_ctx->ev.data = this;
            m_ctx->ev.addRead = RedisQtAddRead;
            m_ctx->ev.delRead = RedisQtDelRead;
            m_ctx->ev.addWrite = RedisQtAddWrite;
            m_ctx->ev.delWrite = RedisQtDelWrite;
            m_ctx->ev.cleanup = RedisQtCleanup;
            return REDIS_OK;
        }

    private:
        void addRead() {
            if (m_read) return;
            m_read = new QSocketNotifier(m_ctx->c.fd, QSocketNotifier::Read, 0);
            connect(m_read, SIGNAL(activated(int)), this, SLOT(read()));
        }

        void delRead() {
            if (!m_read) return;
            delete m_read;
            m_read = 0;
        }

        void addWrite() {
            if (m_write) return;
            m_write = new QSocketNotifier(m_ctx->c.fd, QSocketNotifier::Write, 0);
            connect(m_write, SIGNAL(activated(int)), this, SLOT(write()));
        }

        void delWrite() {
            if (!m_write) return;
            delete m_write;
            m_write = 0;
        }

        void cleanup() {
            delRead();
            delWrite();
        }

    private slots:
        void read() { redisAsyncHandleRead(m_ctx); }
        void write() { redisAsyncHandleWrite(m_ctx); }

    private:
        redisAsyncContext * m_ctx;
        QSocketNotifier * m_read;
        QSocketNotifier * m_write;
};

#endif /* !__HIREDIS_QT_H__ */
