#include "test/jemalloc_test.h"

#define NSENDERS	3
#define NMSGS		100000

typedef struct mq_msg_s mq_msg_t;
struct mq_msg_s {
	mq_msg(mq_msg_t)	link;
};
mq_gen(static, mq_, mq_t, mq_msg_t, link)

TEST_BEGIN(test_mq_basic) {
	mq_t mq;
	mq_msg_t msg;

	expect_false(mq_init(&mq), "Unexpected mq_init() failure");
	expect_u_eq(mq_count(&mq), 0, "mq should be empty");
	expect_ptr_null(mq_tryget(&mq),
	    "mq_tryget() should fail when the queue is empty");

	mq_put(&mq, &msg);
	expect_u_eq(mq_count(&mq), 1, "mq should contain one message");
	expect_ptr_eq(mq_tryget(&mq), &msg, "mq_tryget() should return msg");

	mq_put(&mq, &msg);
	expect_ptr_eq(mq_get(&mq), &msg, "mq_get() should return msg");

	mq_fini(&mq);
}
TEST_END

static void *
thd_receiver_start(void *arg) {
	mq_t *mq = (mq_t *)arg;
	unsigned i;

	for (i = 0; i < (NSENDERS * NMSGS); i++) {
		mq_msg_t *msg = mq_get(mq);
		expect_ptr_not_null(msg, "mq_get() should never return NULL");
		dallocx(msg, 0);
	}
	return NULL;
}

static void *
thd_sender_start(void *arg) {
	mq_t *mq = (mq_t *)arg;
	unsigned i;

	for (i = 0; i < NMSGS; i++) {
		mq_msg_t *msg;
		void *p;
		p = mallocx(sizeof(mq_msg_t), 0);
		expect_ptr_not_null(p, "Unexpected mallocx() failure");
		msg = (mq_msg_t *)p;
		mq_put(mq, msg);
	}
	return NULL;
}

TEST_BEGIN(test_mq_threaded) {
	mq_t mq;
	thd_t receiver;
	thd_t senders[NSENDERS];
	unsigned i;

	expect_false(mq_init(&mq), "Unexpected mq_init() failure");

	thd_create(&receiver, thd_receiver_start, (void *)&mq);
	for (i = 0; i < NSENDERS; i++) {
		thd_create(&senders[i], thd_sender_start, (void *)&mq);
	}

	thd_join(receiver, NULL);
	for (i = 0; i < NSENDERS; i++) {
		thd_join(senders[i], NULL);
	}

	mq_fini(&mq);
}
TEST_END

int
main(void) {
	return test(
	    test_mq_basic,
	    test_mq_threaded);
}

