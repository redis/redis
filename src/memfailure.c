/* ==========================================================================
 * memfailure.c - hardware memory corruption fault handler.
 * --------------------------------------------------------------------------
 * Copyright (C) 2020  zhenwei pi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ==========================================================================
 */
#include "server.h"
#include <assert.h>

#ifdef USE_BUS_MCEERR
void sigbusHandler(int sig, siginfo_t *info, void *secret) {
    int mce;

    /* unlikey, avoid someone to use this handler */
    if (sig != SIGBUS)
        sigsegvHandler(sig, info, secret);

    mce = (info->si_code == BUS_MCEERR_AR) || (info->si_code == BUS_MCEERR_AO);
    if (!mce) {
        /* not a MCE error, try to handle it by sigsegvHandler */
        if (server.crashlog_enabled)
            sigsegvHandler(sig, info, secret);
        else
            return;
    }

    if(info->si_code == BUS_MCEERR_AR) {
        /* fatal error, maybe we can just evict one or several key(s) to recovery redis in the future */
        serverLog(LL_WARNING, "Redis %s crashed by SIGBUS: hardware memory error consumed on a machine check: action required, see detailed message from kernel log", REDIS_VERSION);
        sigsegvHandler(sig, info, secret);
    } else if(info->si_code == BUS_MCEERR_AO) {
        serverLog(LL_NOTICE, "Redis %s caught SIGBUS: hardware memory error detected in process but not consumed: action optional, see detailed message from kernel log", REDIS_VERSION);
    }
}

#else /* USE_BUS_MCEERR */
static void sigbus_raise_default(void)
{
    struct sigaction act;

    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND;
    act.sa_handler = SIG_DFL;
    if (!sigaction(SIGBUS, &act, NULL))
        raise(SIGBUS);
    else
        assert(NULL);
}

/* no MCE supported on this platform, handle SIGBUS by sigsegvHandler or default handler */
void sigbusHandler(int sig, siginfo_t *info, void *secret) {
    if (server.crashlog_enabled)
        sigsegvHandler(sig, info, secret);
    else
        sigbus_raise_default();
}

#endif /* USE_BUS_MCEERR */
