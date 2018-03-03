#ifndef __HIREDIS_EXAMPLE_QT_H
#define __HIREDIS_EXAMPLE_QT_H

#include <adapters/qt.h>

class ExampleQt : public QObject {

    Q_OBJECT

    public:
        ExampleQt(const char * value, QObject * parent = 0)
            : QObject(parent), m_value(value) {}

    signals:
        void finished();

    public slots:
        void run();

    private:
        void finish() { emit finished(); }

    private:
        const char * m_value;
        redisAsyncContext * m_ctx;
        RedisQtAdapter m_adapter;

    friend
    void getCallback(redisAsyncContext *, void *, void *);
};

#endif /* !__HIREDIS_EXAMPLE_QT_H */
