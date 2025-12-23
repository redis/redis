#ifndef RATE_LIMIT_H
#define RATE_LIMIT_H

#include <stdint.h>
#include <time.h>

/* server.c 에 있는 client 구조체 */
struct client;
typedef struct client client;

/* 동시에 추적할 클라이언트 수 (필요하면 조정 가능) */
#define RL_MAX_ENTRIES   2048

typedef struct rl_entry {
    uint64_t id;         /* client->id */
    time_t   window_start;
    int      count;
    int      in_use;
} rl_entry;

/* c: 어떤 클라이언트인지
 * tag: 어느 파트에서 체크하는지 표시용 문자열
 * return: 1 = 허용, 0 = 차단
 */
int clientRateLimitCheck(client *c, const char *tag);

#endif