#include "server.h"        // client, serverLog, catClientInfoString 등
#include "rate_limit.h"
#include <time.h>

static rl_entry rl_table[RL_MAX_ENTRIES];

/* client->id 로 엔트리를 찾거나 새로 만든다 */
static rl_entry *find_or_create(uint64_t id) {
    /* 이미 있는 엔트리 찾기 */
    for (int i = 0; i < RL_MAX_ENTRIES; i++) {
        if (rl_table[i].in_use && rl_table[i].id == id)
            return &rl_table[i];
    }
    /* 빈 슬롯에 새로 만들기 */
    for (int i = 0; i < RL_MAX_ENTRIES; i++) {
        if (!rl_table[i].in_use) {
            rl_table[i].in_use       = 1;
            rl_table[i].id           = id;
            rl_table[i].window_start = 0;
            rl_table[i].count        = 0;
            return &rl_table[i];
        }
    }
    /* 테이블 꽉 찬 경우: 그냥 NULL 리턴 (서버 죽이지 않음) */
    return NULL;
}

/* 클라이언트별 레이트 리밋 체크 + 디버그 로그 */
int clientRateLimitCheck(client *c, const char *tag) {
    /* 전역 설정에서 rate limit 꺼져 있으면 그냥 통과 */
    if (!server.rlimit_enabled) return 1;
    if (!c) return 1;

    rl_entry *ent = find_or_create(c->id);
    if (!ent) return 1;  /* 실패 시 그냥 허용 */

    time_t now = time(NULL);

    /* 윈도우 리셋: window_start가 0이거나, 윈도우 시간이 지났으면 새 윈도우 */
    if (ent->window_start == 0 ||
        now - ent->window_start >= server.rlimit_window_sec)
    {
        ent->window_start = now;
        ent->count = 0;
    }

    /* 현재 윈도우 내 카운트 증가 */
    ent->count++;

    /* 디버그 로그: count / max / window 값을 찍어본다 */
    serverLog(LL_WARNING,
              "RLDBG id=%llu tag=%s count=%d max=%lld win=%lld",
              (unsigned long long)c->id,
              tag ? tag : "(null)",
              ent->count,
              (long long)server.rlimit_max_requests,
              (long long)server.rlimit_window_sec);

    /* 레이트 리밋 체크 */
    if (ent->count > server.rlimit_max_requests) {
        /* 처음 초과할 때만 상세 로그 (로그 폭탄 방지) */
        if (ent->count == server.rlimit_max_requests + 1) {
            sds info = sdsempty();               /* 빈 문자열 */
            info = catClientInfoString(info, c); /* 클라이언트 정보 붙이기 */

            serverLog(LL_WARNING,
                "Rate limit exceeded: client-id=%llu, part=\"%s\", info=%s, "
                "count=%d in %lld sec (limit=%lld)",
                (unsigned long long)c->id,
                tag ? tag : "(unknown)",
                info,
                ent->count,
                (long long)server.rlimit_window_sec,
                (long long)server.rlimit_max_requests);

            sdsfree(info);
        }
        return 0;   /* deny */
    }

    return 1;       /* allow */
}
