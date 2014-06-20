/*
 * Simple templated message queue implementation that relies on only mutexes for
 * synchronization (which reduces portability issues).  Given the following
 * setup:
 *
 *   typedef struct mq_msg_s mq_msg_t;
 *   struct mq_msg_s {
 *           mq_msg(mq_msg_t) link;
 *           [message data]
 *   };
 *   mq_gen(, mq_, mq_t, mq_msg_t, link)
 *
 * The API is as follows:
 *
 *   bool mq_init(mq_t *mq);
 *   void mq_fini(mq_t *mq);
 *   unsigned mq_count(mq_t *mq);
 *   mq_msg_t *mq_tryget(mq_t *mq);
 *   mq_msg_t *mq_get(mq_t *mq);
 *   void mq_put(mq_t *mq, mq_msg_t *msg);
 *
 * The message queue linkage embedded in each message is to be treated as
 * externally opaque (no need to initialize or clean up externally).  mq_fini()
 * does not perform any cleanup of messages, since it knows nothing of their
 * payloads.
 */
#define	mq_msg(a_mq_msg_type)	ql_elm(a_mq_msg_type)

#define	mq_gen(a_attr, a_prefix, a_mq_type, a_mq_msg_type, a_field)	\
typedef struct {							\
	mtx_t			lock;					\
	ql_head(a_mq_msg_type)	msgs;					\
	unsigned		count;					\
} a_mq_type;								\
a_attr bool								\
a_prefix##init(a_mq_type *mq) {						\
									\
	if (mtx_init(&mq->lock))					\
		return (true);						\
	ql_new(&mq->msgs);						\
	mq->count = 0;							\
	return (false);							\
}									\
a_attr void								\
a_prefix##fini(a_mq_type *mq)						\
{									\
									\
	mtx_fini(&mq->lock);						\
}									\
a_attr unsigned								\
a_prefix##count(a_mq_type *mq)						\
{									\
	unsigned count;							\
									\
	mtx_lock(&mq->lock);						\
	count = mq->count;						\
	mtx_unlock(&mq->lock);						\
	return (count);							\
}									\
a_attr a_mq_msg_type *							\
a_prefix##tryget(a_mq_type *mq)						\
{									\
	a_mq_msg_type *msg;						\
									\
	mtx_lock(&mq->lock);						\
	msg = ql_first(&mq->msgs);					\
	if (msg != NULL) {						\
		ql_head_remove(&mq->msgs, a_mq_msg_type, a_field);	\
		mq->count--;						\
	}								\
	mtx_unlock(&mq->lock);						\
	return (msg);							\
}									\
a_attr a_mq_msg_type *							\
a_prefix##get(a_mq_type *mq)						\
{									\
	a_mq_msg_type *msg;						\
	struct timespec timeout;					\
									\
	msg = a_prefix##tryget(mq);					\
	if (msg != NULL)						\
		return (msg);						\
									\
	timeout.tv_sec = 0;						\
	timeout.tv_nsec = 1;						\
	while (true) {							\
		nanosleep(&timeout, NULL);				\
		msg = a_prefix##tryget(mq);				\
		if (msg != NULL)					\
			return (msg);					\
		if (timeout.tv_sec == 0) {				\
			/* Double sleep time, up to max 1 second. */	\
			timeout.tv_nsec <<= 1;				\
			if (timeout.tv_nsec >= 1000*1000*1000) {	\
				timeout.tv_sec = 1;			\
				timeout.tv_nsec = 0;			\
			}						\
		}							\
	}								\
}									\
a_attr void								\
a_prefix##put(a_mq_type *mq, a_mq_msg_type *msg)			\
{									\
									\
	mtx_lock(&mq->lock);						\
	ql_elm_new(msg, a_field);					\
	ql_tail_insert(&mq->msgs, msg, a_field);			\
	mq->count++;							\
	mtx_unlock(&mq->lock);						\
}
