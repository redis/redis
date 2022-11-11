#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "ctrip_util.h"
#include "ctrip_gtid.h"
#include "zmalloc.h"
/* util api */
/**
 * @brief 
 *      
 * @param buf  (string write to target buffer)
 * @param src  (string) 
 * @param len  (write to buffer string len)
 * @return size_t 
 * @example
 *     char A[100];
 *     size_t len = writeBuf(A, "12345", 3);
 *     assert(len == 3);
 *     assert(memcmp(A, "123\0", 4) == 0); 
 *     assert(strcmp(A, "123") == 0);
 *      
 */
size_t writeBuf(char* buf, const char* src, size_t len) {
    memcpy(buf, src, len);
    return len;
}
/**
 * @brief 
 *      stringNew （new)
 *      stringFree (delete)
 * @param src 
 * @param len (used byte)
 * @param max (max byte)
 * @return char* 
 * @example 
 *      
 *      char* str = stringNew("123", 3, 10);
 *      assert(strlen(str) == 3);
 *      stringFree(str);
 *     
 */
char* stringNew(const char* src, int len, int max) {
    char* str = zmalloc(max + 1);
    writeBuf(str, src, len);
    str[len] = '\0'; 
    return str;
}
/**
 * @brief 
 *      stringNew （new)
 *      stringFree (delete)
 * @param str 
 */
void stringFree(char* str) {
    zfree(str);
}

//============== about gtidInterval api ===================

/**
 * @brief 
 *      new a gtidInterval object 
 * @param gno 
 * @return gtidInterval* 
 * @example 
 *       gtidInterval* interval = gtidIntervalNew(1);
 *       assert(interval->gno_start == 1);
 *       assert(interval->gno_end == 1);
 *       assert(interval->next == NULL);
 * 
 */
gtidInterval *gtidIntervalNew(rpl_gno gno) {
    return gtidIntervalNewRange(gno, gno);
}

/**
 * @brief 
 * 
 * @param gtid_interval 
 * @return gtidInterval* 
 * @example 
 *      gtidInterval* src = gtidIntervalNewRange(1,2);
 *      src->next = gtidIntervalNewRange(4,5);
 *      gtidInterval* dump = gtidIntervalDump(src);
 *      assert(dump->gno_start == 1);
 *      assert(dump->gno_end == 2);
 *      assert(dump->next->gno_start == 4);
 *      assert(dump->next->gno_end == 5);
 */
gtidInterval *gtidIntervalDump(gtidInterval* gtid_interval) {
    gtidInterval* dump = gtidIntervalNewRange(gtid_interval->gno_start, gtid_interval->gno_end);
    if(gtid_interval->next != NULL) {
        dump->next = gtidIntervalDump(gtid_interval->next);
    }
    return dump;
}

/**
 * @brief 
 *      gtidIntervalNew/gtidIntervalNewRange/gtidIntervalDecode 
 *      gtidIntervalFree (delete obj)
 * @param interval 
 */
void gtidIntervalFree(gtidInterval* interval) {
    zfree(interval);
}

/**
 * @brief 
 *      new a gtidInterval object 
 * @param start 
 * @param end
 * @return gtidInterval* 
 * @example 
 *       gtidInterval* interval = gtidIntervalNewRange(1, 10);
 *       assert(interval->gno_start == 1);
 *       assert(interval->gno_end == 10);
 *       assert(interval->next == NULL);
 * 
 */
gtidInterval *gtidIntervalNewRange(rpl_gno start, rpl_gno end) {
    gtidInterval *interval = zmalloc(sizeof(*interval));
    interval->gno_start = start;
    interval->gno_end = end;
    interval->next = NULL;
    return interval;
}


/**
 * @brief 
 *      new a gtidInterval object from string
 *      when gno_start == gno_end:
 *           string format is {gno_start}
 *      when gno_start != gno_end:
 *           string format is {gno_start}-{gno_end}
 *      
 * @param gno 
 * @return gtidInterval* 
 * @example 
 *      gtidInterval* interval = gtidIntervalDecode("1", 1);
 *      assert(interval->gno_start == 1);
 *      assert(interval->gno_end == 1);
 *      assert(interval->next == NULL);
 *      gtidIntervalFree(interval);
 *  
 *      interval = gtidIntervalDecode("1-2", 3);
 *      assert(interval->gno_start == 1);
 *      assert(interval->gno_end == 2);
 *      assert(interval->next == NULL);
 *      gtidIntervalFree(interval);
 *
 *      //error 
 *      interval = gtidIntervalDecode("1-2-", 4);
 *      assert(interval == NULL);
 */     
gtidInterval *gtidIntervalDecode(char* interval_str, size_t len) {
    const char *hyphen = "-";
    int count = 0;
    int index = -1;
    for(int i = 0; i < len; i++) {
        if(interval_str[i] == hyphen[0]) {
            index = i;
            break;
        }    
    }
    rpl_gno gno_start = 0, gno_end = 0;
    if(index == -1) {
        if(string2ll(interval_str, len, &gno_start)) {
            return gtidIntervalNew(gno_start);
        } 
    } else {
        //{gno_start}-{gno_end}
        if(string2ll(interval_str, index, &gno_start) && 
         string2ll(interval_str + index + 1, len - index - 1, &gno_end)) {
             return gtidIntervalNewRange(gno_start, gno_end);
        } 
    }
    return NULL;
}


/**
 * @brief 
 * 
 * @param interval 
 * @param buf  
 * @return size_t (buf len)
 *      gtidInterval* interval = gtidIntervalNewRange(1, 10);
 *      char buf[Interval_Encode_MAX_LEN];  //Interval_Encode_MAX_LEN = 43
 *      size_t used_byte = gtidIntervalEncode(interval, buf);
 *      assert(used_byte == 4);
 *      assert(memcmp(buf, "1-10", used_byte) == 0);
 *      gtidIntervalFree(interval);
 *      
 *      interval = gtidIntervalNew(1);
 *      used_byte = gtidIntervalEncode(interval, buf);
 *      assert(used_byte == 1);
 *      assert(memcmp(buf, "1", used_byte) == 0);
 *      gtidIntervalFree(interval);
 * 
 * 
 */
size_t gtidIntervalEncode(gtidInterval* interval, char* buf) {
    size_t len = 0;
    len += ll2string(buf + len, 21, interval->gno_start);
    if(interval->gno_start != interval->gno_end) {
        len += writeBuf(buf + len, "-", 1);
        len += ll2string(buf + len, 21, interval->gno_end);
    }
    return len;
}


//============== about uuidSet api ===================
/**
 * @brief 
 * 
 * @param rpl_sid 
 * @param rpl_sid_len 
 * @param gno 
 * @return uuidSet* 
 *      
 *      uuidSet* uuid_set = uuidSetNew("A", 1, 10);
 *      memcmp(uuid_set->rpl_sid, "A\0", 2);
 *      assert(uuid_set->intervals->gno_start == 10);
 *      assert(uuid_set->intervals->gno_end == 10);
 *      uuidSetFree(uuid_set);
 * 
 *      uuid_set = uuidSetNew("A12345", 1, 10);
 *      assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);
 *      assert(uuid_set->intervals->gno_start == 10);
 *      assert(uuid_set->intervals->gno_end == 10);
 *      uuidSetFree(uuid_set);
 *      
 */
uuidSet *uuidSetNew(const char *rpl_sid, size_t rpl_sid_len, rpl_gno gno) {
    return uuidSetNewRange(rpl_sid, rpl_sid_len, gno, gno);
}

/**
 * @brief 
 * 
 * @param rpl_sid 
 * @param rpl_sid_len 
 * @param start 
 * @param end 
 * @return uuidSet* 
 *      uuidSet* uuid_set = uuidSetNewRange("A", 1, 1, 100);
 *      memcmp(uuid_set->rpl_sid, "A\0", 2);
 *      assert(uuid_set->intervals->gno_start == 1);
 *      assert(uuid_set->intervals->gno_end == 100);
 *      uuidSetFree(uuid_set);
 * 
 *      uuid_set = uuidSetNewRange("A12345", 1, 1, 100);
 *      memcmp(uuid_set->rpl_sid, "A\0", 2);
 *      assert(uuid_set->intervals->gno_start == 1);
 *      assert(uuid_set->intervals->gno_end == 100);
 *      uuidSetFree(uuid_set);
 */
uuidSet *uuidSetNewRange(const char *rpl_sid, size_t rpl_sid_len, rpl_gno start, rpl_gno end) {
    uuidSet *uuid_set = zmalloc(sizeof(*uuid_set));
    uuid_set->rpl_sid = stringNew(rpl_sid, rpl_sid_len, rpl_sid_len);
    uuid_set->intervals = gtidIntervalNewRange(start, end);
    uuid_set->next = NULL;
    return uuid_set;
}

/**
 * @brief 
 *      delete uuidSet Object
 * @param uuid_set 
 * 
 */
void uuidSetFree(uuidSet* uuid_set) {
    stringFree(uuid_set->rpl_sid);
    gtidInterval *cur = uuid_set->intervals;
    while(cur != NULL) {
        gtidInterval *next = cur->next;
        zfree(cur);
        cur = next;
    }
    zfree(uuid_set);
} 


/**
 * @brief 
 *         copy uuid_Set
 * @param uuid_set 
 * @return uuidSet* 
 * @example
 *       char* str = "A:1-2:4-5:7-8";
 *       uuidSet* uuid_set = uuidSetDecode(str, strlen(str));
 *       uuidSet* dump = uuidSetDump(uuid_set);
 *       char buf[100];
 *       size_t len = uuidSetEncode(dump, buf);
 *       assert(strlen(str) == len);
 *       assert(memcmp(str, dump, len) == 0);
 */
uuidSet *uuidSetDump(uuidSet* uuid_set) {
    uuidSet* dump = uuidSetNewRange(uuid_set->rpl_sid, strlen(uuid_set->rpl_sid), uuid_set->intervals->gno_start, uuid_set->intervals->gno_end);
    if(uuid_set->intervals->next != NULL) {
        dump->intervals->next = gtidIntervalDump(uuid_set->intervals->next);
    }
    if(uuid_set->next != NULL) {
        dump->next = uuidSetDump(uuid_set->next);
    }
    return dump;
}

/**
 * @brief 
 *      At present, the data stored in the data structure linked list is in the order from small to large.
 * 
 *
 * @param uuid_set_str 
 * @param len 
 * @return uuidSet* 
 *      uuidSet* uuid_set = uuidSetDecode("A:1:3:5:7", 9);
 *      assert(uuid_set != NULL);
 *      assert(memcmp(uuid_set->rpl_sid, "A\0", 2)== 0);
 *      assert(uuid_set->intervals->gno_start == 1);
 *      assert(uuid_set->intervals->next->gno_start == 3);
 *      assert(uuid_set->intervals->next->next->gno_start == 5);
 *      assert(uuid_set->intervals->next->next->next->gno_start == 7);
 *      assert(uuid_set->intervals->next->next->next->next == NULL);
 *      uuidSetFree(uuid_set);
 */
uuidSet *uuidSetDecode(char* uuid_set_str, int len) {
    const char *colon = ":";
    char* uuid = NULL;
    int uuid_len = 0;
    int start = 0;
    if(uuid_set_str[len - 1] == colon[0]) {
        return NULL;
    }
    uuidSet *uuid_set = NULL;
    int start_index = 0;
    for(int i = 0; i < len; i++) {
        if(uuid_set_str[i] == colon[0]) {
            if(uuid_set == NULL) {
                uuid_set = zmalloc(sizeof(*uuid_set));
                uuid_set->rpl_sid = stringNew(uuid_set_str, i, i);
                uuid_set->intervals = NULL;
                uuid_set->next = NULL;
                start_index = i + 1;
                break;
            } 
        }
    }
    if(uuid_set == NULL) {
        return NULL;
    }
    int end_index = len - 1;
    int count = 0;
    for(int i = len - 2; i >= start_index; i--) {
        if(uuid_set_str[i] == colon[0]) {
            if(i == end_index) {
                goto ERROR;
            }
            gtidInterval* interval = gtidIntervalDecode(uuid_set_str + i + 1, end_index - i);
            interval->next = uuid_set->intervals;
            uuid_set->intervals = interval;
            end_index = i - 1;
            count++;
        }
    }
    gtidInterval* interval = gtidIntervalDecode(uuid_set_str + start_index, end_index - start_index + 1);
    interval->next = uuid_set->intervals;
    uuid_set->intervals = interval;
    return uuid_set;
    ERROR:
        if(uuid_set != NULL) {
            uuidSetFree(uuid_set);
            uuid_set = NULL;
        }
        return NULL;
}

/**
 * @brief 
 *      estimate the maximum length of an uuidSet object converted to a string
 * @param uuid_set 
 * @return size_t 
 *      uuidSet* uuid_set = uuidSetNew("A", 1, 1);
 *      assert(uuidSetEstimatedEncodeBufferSize(uuid_set) > 3);
 *      uuidSetAdd(uuid_set, 3);
 *      assert(uuidSetEstimatedEncodeBufferSize(uuid_set) > 5);
 *      uuidSetAdd(uuid_set, 5);
 *      assert(uuidSetEstimatedEncodeBufferSize(uuid_set) > 7);
 *      uuidSetFree(uuid_set);
 *
 *      char* decode_str = "A:1:3:5:7:9";
 *      uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
 *      size_t max_len = uuidSetEstimatedEncodeBufferSize(uuid_set);
 *      assert(max_len > strlen(decode_str));
 *      uuidSetFree(uuid_set);
 */
size_t uuidSetEstimatedEncodeBufferSize(uuidSet* uuid_set) {
    //{rpl_sid}: {longlong}-{longlong}* n
    size_t intervals_count = 0;
    gtidInterval* current = uuid_set->intervals;
    while(current != NULL) {
        intervals_count++;
        current = current->next;
    }
    return strlen(uuid_set->rpl_sid) + intervals_count * 44; // 44 = 1(:) + 21(longlong) + 1(-) + 21(long long)
}

/**
 * @brief 
 * 
 * @param uuid_set 
 * @param buf 
 * @return size_t 
 *       char *decode_str = "A:1:2:3:4:5";
 *       uuidSet* uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
 *       size_t max_len = uuidSetEstimatedEncodeBufferSize(uuid_set);
 *       char uuid_set_str[max_len];
 *       size_t len = uuidSetEncode(uuid_set, uuid_set_str);
 *       uuid_set_str[len] = '\0';
 *       assert(strcmp(uuid_set_str, decode_str) == 0);
 *       uuidSetFree(uuid_set);
 *      
 * 
 */
size_t uuidSetEncode(uuidSet* uuid_set, char* buf) {
    size_t len = 0;
    len += writeBuf(buf + len, uuid_set->rpl_sid, strlen(uuid_set->rpl_sid));
    gtidInterval *cur = uuid_set->intervals;
    while(cur != NULL) {
        len += writeBuf(buf + len, ":", 1);
        len += gtidIntervalEncode(cur, buf + len);
        cur = cur->next;
    }
    return len;
}

/**
 * @brief 
 * 
 * @param uuid_set 
 * @param gtidInterval 
 * @return int 
 * @example
 *       decode_str = "A:4-5:7-8:10-11";
 *       uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
 *       interval = gtidIntervalNewRange(1,2);
 *       assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
 *       buf_len = uuidSetEncode(uuid_set, buf);
 *       assert(buf_len == strlen("A:1-2:4-5:7-8:10-11"));
 *       assert(strncmp(buf, "A:1-2:4-5:7-8:10-11", buf_len) == 0);
 */
#define min(a, b)	(a) < (b) ? a : b
#define max(a, b)	(a) < (b) ? b : a
int uuidSetAddGtidInterval(uuidSet* uuid_set, gtidInterval* interval) {
    gtidInterval *cur = uuid_set->intervals;
    gtidInterval *next = cur->next;
    gtidInterval* start_in_interval;
    gtidInterval* end_in_interval;
    /**
     * @brief 
     *      A:0  +  A:1-10  = A:1-10
     */
    if (cur->gno_start == 0 && cur->gno_end == 0) {
        cur->gno_start = interval->gno_start;
        cur->gno_end = interval->gno_end;
        return 1;
    }
    /**
     * @brief 
     *     A:4-5:7-8:10-11  + A:1-2 = A:1-2:4-5:7-8:10-11
     */
    if (interval->gno_end < cur->gno_start - 1) {
        uuid_set->intervals = gtidIntervalDump(interval);
        uuid_set->intervals->next = cur;
        return 1;
    }
    int changed = 0;
    char* error_scope;
    do {
        //  A  {cur->gno_start} B {cur->gno_end} C {next->gno_start} D {next->gno_end} E
        //  next B = D  
        if(interval->gno_start < cur->gno_start - 1) {
            if(interval->gno_end < cur->gno_start - 1) {
                //A-A
                //ignore
                //exec C-C
                error_scope = "A-A";
                goto Error;
            } else if(interval->gno_end <= cur->gno_end + 1){
                //A-B
                cur->gno_start = interval->gno_start;
                cur->gno_end = max(interval->gno_end, cur->gno_end);
                return 1;
            } else if(next == NULL || (next != NULL && interval->gno_end < next->gno_start - 1)) {
                //A-C
                cur->gno_start = interval->gno_start;
                cur->gno_end = interval->gno_end;
                return 1;
            } else if(interval->gno_end <= next->gno_end + 1) {
                //A-D
                cur->gno_start = interval->gno_start;
                cur->gno_end = max(next->gno_end, interval->gno_end);
                cur->next = next->next;
                gtidIntervalFree(next);
                return 1;
            } else {
                //A-E
                cur->gno_end = next->gno_end;
                cur->next = next->next;
                gtidIntervalFree(next);
                next = cur->next;
                changed = 1;
                continue;
            } 

        } else if(interval->gno_start <= cur->gno_end + 1) {
            //B
            if(interval->gno_end < cur->gno_start - 1) {
                //B-A
                //ignore
                error_scope = "B-A";
                goto Error;
            } else if(interval->gno_end <= cur->gno_end + 1){
                //B-B
                //ignore
                long long start_min = min(interval->gno_start, cur->gno_start);
                long long end_max = max(interval->gno_end, cur->gno_end);
                if (start_min != cur->gno_start || end_max != cur->gno_end) {
                    changed = 1;
                }
                cur->gno_start = start_min;
                cur->gno_end = end_max;
                if(next != NULL && cur->gno_end == next->gno_start - 1) {
                    cur->gno_end = next->gno_end;
                    cur->next = next->next;
                    gtidIntervalFree(next);
                }
                return changed;
            } else if(next == NULL || interval->gno_end < next->gno_start - 1) {
                //B-C
                cur->gno_start = min(interval->gno_start, cur->gno_start);
                cur->gno_end = interval->gno_end;
                return 1;
            } else if(interval->gno_end <= next->gno_end + 1){
                //B-D
                cur->gno_start = min(interval->gno_start, cur->gno_start);
                cur->gno_end = max(next->gno_end, interval->gno_end);
                cur->next = next->next;
                gtidIntervalFree(next);
                return 1;
            } else {
                //B-E
                cur->gno_start = min(interval->gno_start, cur->gno_start);
                cur->gno_end = next->gno_end;
                cur->next = next->next;
                gtidIntervalFree(next);
                next = cur->next;
                changed = 1;
                continue;
            }
        } else if(next == NULL || interval->gno_end < next->gno_start - 1) {
            //C
            if(interval->gno_end < cur->gno_start - 1) {
                //ignore
                //C-A
                error_scope = "C-A";
                goto Error;
            } else if(interval->gno_end <= cur->gno_end + 1){
                //C-B
                error_scope = "C-B";
                goto Error;
            } else if(next == NULL || interval->gno_end < next->gno_start - 1) {
                //C-C
                gtidInterval* new_next = gtidIntervalDump(interval);
                new_next->next = cur->next;
                cur->next = new_next;
                return 1;
            } else if(interval->gno_end <= next->gno_end + 1){
                //C-D
                next->gno_start = cur->gno_start;
                return 1;
            } else {
                //C-E
                next->gno_start = cur->gno_start;
                changed = 1;
            }
        } else {
            //ignore
            // 1. gno_start <= gno_end   =>   B * A = 0(not exist)  
            //     （A,B) * (A,B) = A* A + A* B + B*B 
            // 2. D = (next B), E = (next C)
            // 3. (A, B, C, D ,E) * (A, B, C, D ,E)
            //        = (A,B,C) * (A,B,C,D,E)  + D * D + D * E + E * E
            //        = (A,B,C) * (A,B,C,D,E)  + next (B * B) + next (B * C) + next(C * C)
            //        <  (A,B,C) * (A,B,C,D,E)  + next ((A,B,C) * (A,B,C,D,E))

        }
        
        cur = next;
        if (cur!= NULL) {
            next = cur->next;
        }
    }while(cur != NULL);
Error:
    printf("\n code error [%s] %lld-%lld",error_scope, interval->gno_start, interval->gno_end);
    printf("cur %lld-%lld\n", cur->gno_start, cur->gno_end);
    if(next != NULL) {
        printf("next %lld-%lld\n", next->gno_start, next->gno_end);
    }
    exit(0);
}
int uuidSetAddGtidInterval1(uuidSet* uuid_set, gtidInterval* interval) {
    gtidInterval *cur = uuid_set->intervals;
    gtidInterval *next = cur->next;
    gtidInterval* start_in_interval;
    gtidInterval* end_in_interval;
    /**
     * @brief 
     *      cur (0)  +  interval(1-10)  = cur(1-10)
     */
    if (cur->gno_start == 0 && cur->gno_end == 0) {
        cur->gno_start = interval->gno_start;
        cur->gno_end = interval->gno_end;
        return 1;
    }
    /**
     * @brief 
     *      add head
     */
    if (interval->gno_end < cur->gno_start - 1) {
        uuid_set->intervals = gtidIntervalDump(interval);
        uuid_set->intervals->next = cur;
        return 1;
    }
    int changed = 0;
    do {
        //B
        if (cur->gno_start <= interval->gno_start &&
             cur->gno_end >= interval->gno_end ) {
            return changed;
        }
        //C
        if (cur->gno_end + 1 < interval->gno_start) {
            if( (next != NULL && interval->gno_end < next->gno_start - 1)
                || next == NULL) {
                gtidInterval* dump = gtidIntervalDump(interval);
                dump->next = next;
                cur->next = dump;
                return 1;
            }
            if(next != NULL && interval->gno_start <= next->gno_start -1) {
                if(interval->gno_end > next->gno_start) {
                    next->gno_start = interval->gno_start;
                }
            }
        } else {
            if(next == NULL) {
                cur->gno_end = max(cur->gno_end, interval->gno_end);
            }
        }

        

        //A
        if(next != NULL && 
            interval->gno_start > cur->gno_end && 
            interval->gno_start < next->gno_start && 
            interval->gno_end > next->gno_start &&
            interval->gno_end < next->gno_end
        ) {
            next->gno_start = interval->gno_start;
            return 1;
        }
        //AB
        /**
         * @brief 
         *   A:1 + A:2 = A:1-2
         *   A:1-3 + A:2-4 = A:1-4
         */
        if(interval->gno_start < cur->gno_start &&
            interval->gno_end >= cur->gno_start - 1 &&
            interval->gno_end < cur->gno_end) {
            cur->gno_start = interval->gno_start;
            return 1;
        }

        //BC
        if(interval->gno_start > cur->gno_start && 
            interval->gno_start < cur->gno_end &&
            interval->gno_end > cur->gno_end &&
            next != NULL && 
            interval->gno_end < next->gno_start) {
            cur->gno_end = interval->gno_end;
            return 1;
        }
        //BD
        if(interval->gno_start >= cur->gno_start && 
            interval->gno_start <= cur->gno_end + 1 &&
            next != NULL &&
            interval->gno_end >= next->gno_start - 1 &&
            interval->gno_end <= next->gno_end) {
            cur->gno_end = next->gno_end;
            cur->next = next->next;
            gtidIntervalFree(next);
            return 1;
        }
        //AC
        if(interval->gno_start <= cur->gno_start && 
            interval->gno_end > cur->gno_end &&
            (
                next == NULL || 
                (next != NULL && interval->gno_end < next->gno_start)
            ) 
        ) {
            cur->gno_start = interval->gno_start;
            cur->gno_end = interval->gno_end;
            return 1;
        }
        
        if (next != NULL && 
            interval->gno_start < cur->gno_end && 
            interval->gno_end > next->gno_start
        ) {
            cur->gno_start = min(interval->gno_start, cur->gno_start);
            cur->gno_end = next->gno_end;
            cur->next = next->next;
            gtidIntervalFree(next);
            next = cur->next;
            changed = 1;
            continue;
        } 
        if(interval->gno_start <= cur->gno_end && 
            interval->gno_end >= cur->gno_end && 
            next == NULL) {
            cur->gno_end = interval->gno_end;
        }
        cur = next;
        if (cur!= NULL) {
            next = cur->next;
        }
        
    } while(cur != NULL);
    
    return 1;
}

/**
 * @brief 
 * 
 * @param uuid_set 
 * @param gno 
 * @return int 
 * @example 
 *       uuidSet* uuid_set = uuidSetNew("A", 1, 1);
 *       uuidSetAdd(uuid_set, 3);
 *       assert(uuid_set->intervals->next->gno_start == 3);
 *       assert(uuid_set->intervals->next->gno_end == 3);
 * 
 *       char buf[100];
 *       size_t len = uuidSetEncode(uuid_set, buf);
 *       assert(len == 5); //A:1:3
 *       assert(strncmp(buf, "A:1:3", 5) == 0); 
 */
int uuidSetAdd(uuidSet* uuid_set, rpl_gno gno)  {
    gtidInterval interval = {
        .gno_start = gno,
        .gno_end = gno,
        .next = NULL
    };
    return uuidSetAddGtidInterval(uuid_set, &interval);
}
int uuidSetAdd1(uuidSet* uuid_set, rpl_gno gno) {
    gtidInterval *cur = uuid_set->intervals;
    gtidInterval *next = cur->next;
    if (cur->gno_start == 0 && cur->gno_end == 0) {
        cur->gno_start = gno;
        cur->gno_end = gno;
        return 1;
    }
    if (gno < cur->gno_start - 1) {
        uuid_set->intervals = gtidIntervalNew(gno);
        uuid_set->intervals->next = cur;
        return 1;
    }
    if (gno == cur->gno_start - 1) {
        cur->gno_start = gno;
        return 1;
    }
    while(next != NULL) {
        if (gno == cur->gno_end + 1) {
            if (gno == next->gno_start - 1) {
                cur->gno_end = next->gno_end;
                cur->next = next->next;
                zfree(next);
                return 1;
            } else {
                cur->gno_end = gno;
                return 1;
            }
        }
        if (gno == next->gno_start - 1) {
            next->gno_start = gno;
            return 1;
        }
        if (gno < next->gno_start - 1 && gno > cur->gno_end + 1) {
            cur->next = gtidIntervalNew(gno);
            cur->next->next = next;
            return 1;
        }
        if (gno <= next->gno_end) {
            return 0;
        }
        cur = next;
        next = cur->next;
    }
    if (gno == cur->gno_end + 1) {
        cur->gno_end = gno;
        return 1;
    }
    if (gno > cur->gno_end + 1) {
        cur->next = gtidIntervalNew(gno);
        return 1;
    }
    return 0;
}

/**
 * @brief 
 * 
 * @param uuid_set 
 * @param watermark 
 * @example 
 *      
 */
void uuidSetRaise(uuidSet *uuid_set, rpl_gno watermark) {
    gtidInterval *cur = uuid_set->intervals;
    if (watermark < cur->gno_start - 1) {
        uuid_set->intervals = gtidIntervalNewRange(1, watermark);
        uuid_set->intervals->next = cur;
        return;
    }

    while (cur != NULL) {
        if (watermark > cur->gno_end + 1) {
            gtidInterval *temp = cur;
            cur = cur->next;
            zfree(temp);
            continue;
        }

        if (watermark == cur->gno_end + 1) {
            if (cur->next == NULL) {
                cur->gno_start = 1;
                cur->gno_end = watermark;
                uuid_set->intervals = cur;
                break;
            }
            if (watermark == cur->next->gno_start - 1) {
                gtidInterval *prev = cur;
                cur = cur->next;
                zfree(prev);
                cur->gno_start = 1;
                break;
            } else {
                cur->gno_end = watermark;
                cur->gno_start = 1;
                break;
            }
        }
        if (watermark < cur->gno_start - 1) {
            gtidInterval *temp = cur;
            cur = gtidIntervalNewRange(1, watermark);
            cur->next = temp;
            break;
        } else {
            cur->gno_start = 1;
            break;
        }
    }
    if (cur == NULL) {
        uuid_set->intervals = gtidIntervalNewRange(1, watermark);
    } else {
        uuid_set->intervals = cur;
    }
}

/**
 * @brief 
 * 
 * @param uuid_set 
 * @param gno 
 * @return int 
 *       uuidSet* uuid_set = uuidSetNewRange("A", 1, 1, 5);
 *       assert(uuidSetContains(uuid_set, 1) == 1);
 *       assert(uuidSetContains(uuid_set, 6) == 0);
 */
int uuidSetContains(uuidSet* uuid_set, rpl_gno gno) {
    gtidInterval *cur = uuid_set->intervals;
    while (cur != NULL) {
        if (gno >= cur->gno_start && gno <= cur->gno_end) {
            return 1;
        }
        cur = cur->next;
    }
    return 0;
}


/**
 * @brief 
 * 
 * @param uuid_set 
 * @param updateBeforeReturn 
 * @return rpl_gno 
 * @example
 *      uuidSet* uuid_set = uuidSetNewRange("A", 1, 1, 5);
 *      assert(6 == uuidSetNext(uuid_set, 1));
 *      uuidSetRaise(uuid_set, 8);
 *      assert(9 == uuidSetNext(uuid_set, 1));
 */
rpl_gno uuidSetNext(uuidSet* uuid_set, int updateBeforeReturn) {
    if (uuid_set->intervals == NULL) {
        if (updateBeforeReturn) {
            uuid_set->intervals = gtidIntervalNew(1);
        }
        return 1;
    }

    rpl_gno next;
    if (uuid_set->intervals->gno_start != 1) {
        next = 1;
    } else {
        next = uuid_set->intervals->gno_end + 1;
    }
    if (updateBeforeReturn) {
        uuidSetAdd(uuid_set, next);
    }
    return next;
}

/**
 * @brief 
 * 
 * @param uuid_set 
 * @param updateBeforeReturn 
 * @param buf 
 * @return size_t 
 *      uuidSet* uuid_set = uuidSetNewRange("A", 1, 1, 5);
 *       char buf[100];
 *       size_t len = uuidSetNextEncode(uuid_set, 1, buf);
 *       assert(3 == len);
 *       assert(strncmp(buf, "A:6", len) == 0);
 *       uuidSetRaise(uuid_set, 8);
 *       len = uuidSetNextEncode(uuid_set, 1, buf);
 *       assert(3 == len);
 *       assert(strncmp(buf, "A:9", len) == 0);
 */
size_t uuidSetNextEncode(uuidSet* uuid_set, int updateBeforeReturn, char* buf) {
    size_t len = 0;
    len += writeBuf(buf + len, uuid_set->rpl_sid, strlen(uuid_set->rpl_sid));
    rpl_gno gno = uuidSetNext(uuid_set, updateBeforeReturn);
    len += writeBuf(buf + len, ":", 1);
    len += ll2string(buf + len, 21, gno);
    return len;
}

/**
 * @brief 
 * 
 * @param uuid_set 
 * @param other
 * @example
 *      char* targe_str = "A:1:3:5:7"; 
 *      uuidSet* uuid_set =  uuidSetDecode(targe_str, strlen(targe_str));
 *      char* other_str = "A:2:4:6:8";
 *      uuidSet* other = uuidSetDecode(other_str, strlen(other_str));
 *      uuidSetAppendUuidSet(uuid_set, other);
 *      assert(uuid_set->intervals->gno_start == 1);
 *      assert(uuid_set->intervals->gno_end == 8);
 *      assert(uuid_set->next == NULL);
 *      char buf[100];
 *      size_t len = uuidSetEncode(uuid_set, buf);
 *      assert(strncmp("A:1-8", buf, len) == 0);
 */
int uuidSetAppendUuidSet(uuidSet* uuid_set, uuidSet* other) {
    if(strcmp(uuid_set->rpl_sid, other->rpl_sid) != 0) {
        return 0;
    }
    gtidInterval* interval =  other->intervals;
    while(interval != NULL) {
        uuidSetAddGtidInterval(uuid_set, interval);
        interval = interval->next;
    }
    return 1;
}

//============== about gtidSet api ===================
/**
 * @brief 
 *      new empty gtidSet
 * @return gtidSet* 
 * @example
 *      gtidSet* gtid_set = gtidSetNew();
 *      assert(gtid_set->uuid_sets == NULL);
 *      assert(gtid_set->tail == NULL);
 */
gtidSet* gtidSetNew() {
    gtidSet *gtid_set = zmalloc(sizeof(*gtid_set));
    gtid_set->uuid_sets = NULL;
    gtid_set->tail = NULL;
    return gtid_set;
}

/**
 * @brief 
 *      delete gtidSet Object
 * @param gtid_set 
 * 
 *      
 */
void gtidSetFree(gtidSet *gtid_set) {
    uuidSet *next;
    uuidSet *cur = gtid_set->uuid_sets;
    while(cur != NULL) {
        next = cur->next;
        uuidSetFree(cur);
        cur = next;
    }
    zfree(gtid_set);
}

/**
 * @brief 
 * 
 * @param gtid_set 
 * @param uuid_set 
 *      gtidSet* gtid_set = gtidSetNew();
 *      uuidSet* uuid_set = uuitSetNew("A", 1, 1, 2);
 *      gtidSetAppendUuidSet(gtid_set, uuid_set);
 *      assert(gtid_set->uuid_sets == uuid_set);
 */
void gtidSetAppendUuidSet(gtidSet *gtid_set, uuidSet *uuid_set) {
    if (gtid_set->uuid_sets == NULL) {
        gtid_set->uuid_sets = uuid_set;
        gtid_set->tail = uuid_set;
    } else {
        gtid_set->tail->next = uuid_set;
        gtid_set->tail = uuid_set;
    }
}

/**
 * @brief 
 * 
 * @param src (string to gtidSet object)
 * @return gtidSet* 
 * @example 
 *       char* str = "A:1,B:1";
 *       gtidSet* gtid_set = gtidSetDecode(str, 7);
 *       assert(strcmp(gtid_set->uuid_sets->rpl_sid, "A") == 0);
 *       assert(gtid_set->uuid_sets->intervals->gno_start == 1);
 *       assert(gtid_set->uuid_sets->intervals->gno_end == 1);
 *       assert(strcmp(gtid_set->uuid_sets->next->rpl_sid, "B") == 0);
 *       assert(gtid_set->uuid_sets->next->intervals->gno_start == 1);
 *       assert(gtid_set->uuid_sets->next->intervals->gno_end == 1);
 *   
 */
gtidSet *gtidSetDecode(char* src, size_t len) {
    gtidSet* gtid_set = gtidSetNew();
    const char *split = ",";
    int count = 0;
    int index = 0;
    int uuid_str_start_index = 0;
    for(int i = 0; i < len; i++) {
        if(src[i] == split[0]) {
            uuidSet *uuid_set = uuidSetDecode(src + uuid_str_start_index, i - uuid_str_start_index);
            gtidSetAppendUuidSet(gtid_set, uuid_set);
            uuid_str_start_index = (i + 1);
        }
    }
    uuidSet *uuid_set = uuidSetDecode(src + uuid_str_start_index, len - uuid_str_start_index);
    gtidSetAppendUuidSet(gtid_set, uuid_set);
    return gtid_set;
}


size_t gtidSetEstimatedEncodeBufferSize(gtidSet* gtid_set) {
    size_t max_len = 1; // must > 0; 
    uuidSet *cur = gtid_set->uuid_sets;
    while(cur != NULL) {
        max_len += uuidSetEstimatedEncodeBufferSize(cur) + 1;
        cur = cur->next;
    }
    return max_len;
}
/**
 * @brief 
 * 
 * @param gtid_set (gtidSet object)
 * @param buf (gtidSet object to string and write buf)
 * @return size_t (write to buffer size)
 * @example 
 *   gtidSet* gtid_set = gtidSetNew();
 *   gtidSetAdd(gtid_set, "A", 1);
 *   char buf[5];
 *   size_t size = gtidSetEncode(gtid_set, buf);
 *   assert(size == 10);
 *   assert(strcmp(buf, "A:1"))
 */
size_t gtidSetEncode(gtidSet* gtid_set, char* buf) {
    size_t len = 0; 
    uuidSet *cur = gtid_set->uuid_sets;
    while(cur != NULL) {
        len += uuidSetEncode(cur, buf + len);
        cur = cur->next;
        if (cur != NULL) {
            len +=  writeBuf(buf + len, ",", 1);
        }
    }
    return len;
}

/**
 * @brief 
 * 
 * @param gtid_set 
 * @param rpl_sid 
 * @param len 
 * @return uuidSet* 
 * @example
 *      gtidSet* gtid_set = gtidSetDecode("A:1,B:2", 7);
 *      gtidSet* A = gtidSetFindUuidSet(gtid_set, "A", 1);
 *      assert(A != NULL);
 *      assert(strcmp(A->rpl_sid, "A") == 0);
 *      assert(A->gno_start == 1);
 *      gtidSet* B = gtidSetFindUuidSet(gtid_set, "B", 1);
 *      assert(B != NULL);
 *      assert(strcmp(B->rpl_sid, "B") == 0);
 *      assert(B->gno_start == 2);
 *      
 */
uuidSet* gtidSetFindUuidSet(gtidSet* gtid_set, const char* rpl_sid, size_t len) {
    uuidSet *cur = gtid_set->uuid_sets;
    while(cur != NULL) {
        if (strncmp(cur->rpl_sid, rpl_sid, len) == 0) {
            break;
        }
        cur = cur->next;
    }
    return cur;
}

/**
 * @brief 
 * 
 * @param gtid_set 
 * @param rpl_sid 
 * @param rpl_sid_len 
 * @param gno 
 * @return int 
 *      gtidSet* gtid_set = gtidSetNew();
 *      gtidSetAdd(gtid_set, "A", 1, 1);
 *      assett(gtid_set)
 *      gtidSetAdd(gtid_set, "B", 1, 1);
 *      
 */
int gtidSetAdd(gtidSet* gtid_set, const char* rpl_sid, size_t rpl_sid_len ,rpl_gno gno) {
    uuidSet *cur = gtidSetFindUuidSet(gtid_set, rpl_sid, rpl_sid_len);
    if (cur == NULL) {
        cur = uuidSetNew(rpl_sid, rpl_sid_len, gno);
        gtidSetAppendUuidSet(gtid_set, cur);
        return 1;
    } else {
        return uuidSetAdd(cur, gno);
    }
}

/**
 * @brief 
 * 
 * @param src 
 * @param gno 
 * @param rpl_sid_len 
 * @return char* (rpl_sid pointer)
 * 
 *       long long gno = 0;
 *       long long uuid_index = 0;
 *       char* uuid = uuidDecode("ABCD:1", 6, &gno, &uuid_index);
 *       assert(uuid_index == 4);
 *       assert(strncmp(uuid, "ABCD", uuid_index) == 0);
 *       assert(gno == 1);
 */
char* uuidDecode(char* src, size_t src_len, long long* gno, int* rpl_sid_len) {
    const char *split = ":";
    int index = -1;
    for(int i = 0; i < src_len; i++) {
        if(src[i] == split[0]) {
            index = i;
            break;
        }
    }
    if(index == -1) {
        return NULL;
    }
    if(string2ll(src + index + 1, src_len - index - 1, gno) == 0) {
        return NULL;
    } 
    *rpl_sid_len = index;
    return src;
}

/**
 * @brief 
 * 
 * @param gtid_set 
 * @param rpl_sid 
 * @param rpl_sid_len 
 * @param watermark 
 * @example
 *      char* gtid_set_str = "A:1:3:5:7";
 *      gtidSet* gtid_set = gtidSetDecode(gtid_set_str, strlen(gtid_set_str));
 *      gtidSetRaise(gtid_set, "A", 1, 10);
 *      assert(gtid_set->uuid_sets->intervals->gno_start == 1);
 *      assert(gtid_set->uuid_sets->intervals->gno_end == 10);
 *      gtidSetFree(gtid_set);
 */
void gtidSetRaise(gtidSet* gtid_set, const char* rpl_sid, size_t rpl_sid_len, rpl_gno watermark) {
    if (watermark == 0) return;
    uuidSet *cur = gtidSetFindUuidSet(gtid_set, rpl_sid, rpl_sid_len);
    if (cur == NULL) {
        cur = uuidSetNewRange(rpl_sid, rpl_sid_len, 1, watermark);
        gtidSetAppendUuidSet(gtid_set, cur);
    } else {
        uuidSetRaise(cur, watermark);
    }
}


/**
 * @brief 
 *      
 * @param gtid_set 
 * @param other 
 * @example 
 *      char* A_str = "A:1:3:5:7";
 *      gtidSet* A = gtidSetDecode(A_str, strlen(A_str));
 *      char* B_str = "B:1:3:5:7";
 *      gtidSet* B = gtidSetDecode(B_str, strlen(B_str));
 *      gtidSetAppendGtidSet(A, B);
 *      //A = "A:1:3:5:7,B:1:3:5:7"
 *      //B = "B:1:3:5:7"
 */
void gtidSetAppendGtidSet(gtidSet* gtid_set, gtidSet* other) {
    uuidSet *cur = other->uuid_sets;
    while(cur != NULL) {
        uuidSet* src = gtidSetFindUuidSet(gtid_set, cur->rpl_sid, strlen(cur->rpl_sid)); 
        if(src != NULL) {
            uuidSetAppendUuidSet(src, cur);
        } else {
            gtidSetAppendUuidSet(gtid_set, uuidSetDump(cur));
        }
        cur = cur->next;
    }
}