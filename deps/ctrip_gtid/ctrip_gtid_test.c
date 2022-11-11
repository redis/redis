#include "ctrip_gtid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testhelp.h"
#include "limits.h"
#include "zmalloc.h"
#include "ctrip_util.h"
#define assert(e) do {							\
	if (!(e)) {				\
		printf(						\
		    "%s:%d: Failed assertion: \"%s\"\n",	\
		    __FILE__, __LINE__, #e);				\
		return 0;						\
	}								\
} while (0)

#ifdef CTRIP_GTID_TEST
    
    int test_writeBuf() {
        char A[100];
        size_t len = writeBuf(A, "12345", 3);
        assert(len == 3);
        assert(strncmp(A, "123", len) == 0);
        return 1;
    }

    int test_stringNew() {
        char* str = stringNew("123", 3, 10);
        assert(strlen(str) == 3);
        stringFree(str);
        return 1;
    }

    int test_gtidSetAppendUuidSet() {
        gtidSet* gtid_set = gtidSetNew();
        uuidSet* uuid_set = uuidSetNewRange("A", 1, 1, 2);
        gtidSetAppendUuidSet(gtid_set, uuid_set);
        assert(gtid_set->uuid_sets == uuid_set);
        uuidSet* uuid_set1 = uuidSetNewRange("A", 1, 3, 4);
        gtidSetAppendUuidSet(gtid_set, uuid_set1);
        assert(gtid_set->uuid_sets == uuid_set);
        return 1;
    }

#endif
    int test_gtidIntervalNew() {
        gtidInterval* interval = gtidIntervalNew(1);
        assert(interval->gno_start == 1);
        assert(interval->gno_end == 1);
        assert(interval->next == NULL);
        gtidIntervalFree(interval);

        interval = gtidIntervalNew(-__LONG_LONG_MAX__);
        assert(interval->gno_start == -__LONG_LONG_MAX__);
        assert(interval->gno_end == -__LONG_LONG_MAX__);
        assert(interval->next == NULL);
        gtidIntervalFree(interval);

        interval = gtidIntervalNew(__LONG_LONG_MAX__);
        assert(interval->gno_start == __LONG_LONG_MAX__);
        assert(interval->gno_end == __LONG_LONG_MAX__);
        assert(interval->next == NULL);
        return 1;
    }

    int test_gtidIntervalNewRange() {
        gtidInterval* interval = gtidIntervalNewRange(1, 10);
        assert(interval->gno_start == 1);
        assert(interval->gno_end == 10);
        assert(interval->next == NULL);
        gtidIntervalFree(interval);

        interval = gtidIntervalNewRange(-__LONG_LONG_MAX__, __LONG_LONG_MAX__);
        assert(interval->gno_start == -__LONG_LONG_MAX__);
        assert(interval->gno_end == __LONG_LONG_MAX__);
        assert(interval->next == NULL);
        gtidIntervalFree(interval);
        return 1;
    }

    int test_gtidIntervalDump() {
        gtidInterval* src = gtidIntervalNewRange(1,2);
        src->next = gtidIntervalNewRange(4,5);
        gtidInterval* dump = gtidIntervalDump(src);
        assert(dump->gno_start == 1);
        assert(dump->gno_end == 2);
        assert(dump->next->gno_start == 4);
        assert(dump->next->gno_end == 5);
        return 1;
    }

    int test_gtidIntervalDecode() {
        gtidInterval* interval = gtidIntervalDecode("7", 1);
        assert(interval->gno_start == 7);
        assert(interval->gno_end == 7);
        assert(interval->next == NULL);
        gtidIntervalFree(interval);
  
        interval = gtidIntervalDecode("1-9", 3);
        assert(interval->gno_start == 1);
        assert(interval->gno_end == 9);
        assert(interval->next == NULL);
        gtidIntervalFree(interval);
  
        //error 
        interval = gtidIntervalDecode("1-2-", 4);
        assert(interval == NULL);
        gtidIntervalFree(interval);

        return 1;
    }

    int test_gtidIntervalEncode() {
        gtidInterval* interval = gtidIntervalNewRange(1, 10);
        char buf[Interval_Encode_MAX_LEN];  //Interval_Encode_MAX_LEN = 43
        size_t used_byte = gtidIntervalEncode(interval, buf);
        assert(used_byte == 4);
        assert(memcmp(buf, "1-10", used_byte) == 0);
        gtidIntervalFree(interval);
       
        interval = gtidIntervalNew(1);
        used_byte = gtidIntervalEncode(interval, buf);
        assert(used_byte == 1);
        assert(memcmp(buf, "1", used_byte) == 0);
        gtidIntervalFree(interval);

        interval = gtidIntervalDecode("712", 1);
        used_byte = gtidIntervalEncode(interval, buf);
        assert(strncmp(buf, "7", used_byte) == 0);
        return 1;
    }

    int test_uuidSetNew() {
        uuidSet* uuid_set = uuidSetNew("A", 1, 10);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);
        assert(uuid_set->intervals->gno_start == 10);
        assert(uuid_set->intervals->gno_end == 10);
        uuidSetFree(uuid_set);

        uuid_set = uuidSetNew("A12345", 1, 10);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);
        assert(uuid_set->intervals->gno_start == 10);
        assert(uuid_set->intervals->gno_end == 10);
        uuidSetFree(uuid_set);
        return 1;
    }

    int test_uuidSetNewRange() {
        uuidSet* uuid_set = uuidSetNewRange("A", 1, 1, 100);
        memcmp(uuid_set->rpl_sid, "A\0", 2);
        assert(uuid_set->intervals->gno_start == 1);
        assert(uuid_set->intervals->gno_end == 100);
        uuidSetFree(uuid_set);
   
        uuid_set = uuidSetNewRange("A12345", 1, 1, 100);
        memcmp(uuid_set->rpl_sid, "A\0", 2);
        assert(uuid_set->intervals->gno_start == 1);
        assert(uuid_set->intervals->gno_end == 100);
        uuidSetFree(uuid_set);

        uuid_set = uuidSetNewRange("A", 1, 1, 9);
        //"Create an new uuid set with 1-9"
        assert(uuid_set->intervals->gno_start == 1);
        assert(uuid_set->intervals->gno_end == 9);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);
        uuidSetFree(uuid_set);
        return 1;
    }

    int test_uuidSetDump() {
        char* str = "A:1-2:4-5:7-8";
        uuidSet* uuid_set = uuidSetDecode(str, strlen(str));
        uuidSet* dump = uuidSetDump(uuid_set);
        char buf[100];
        size_t len = uuidSetEncode(dump, buf);
        buf[len] = '\0';
        printf("\n%s\n", buf);
        assert(strlen(str) == len);
        assert(memcmp(str, buf, len) == 0);
        return 1;
    }

    int test_uuidSetDecode() {
        uuidSet* uuid_set = uuidSetDecode("A:1:3:5:7", 9);
        assert(uuid_set != NULL);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2)== 0);
        assert(uuid_set->intervals->gno_start == 1);
        assert(uuid_set->intervals->next->gno_start == 3);
        assert(uuid_set->intervals->next->next->gno_start == 5);
        assert(uuid_set->intervals->next->next->next->gno_start == 7);
        assert(uuid_set->intervals->next->next->next->next == NULL);
        uuidSetFree(uuid_set);

        uuid_set = uuidSetDecode("A:1-6:8", 7);
        assert(uuid_set->intervals->gno_start == 1);
        assert(uuid_set->intervals->gno_end == 6);
        assert(uuid_set->intervals->next->gno_start == 8);
        assert(uuid_set->intervals->next->gno_end == 8);
        assert(uuid_set->intervals->next->next == NULL);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);
        uuidSetFree(uuid_set);

        uuid_set = uuidSetDecode("A:2-5:9adbsdada", 7);
        assert(uuid_set->intervals->gno_start == 2);
        assert(uuid_set->intervals->gno_end == 5);
        assert(uuid_set->intervals->next->gno_start == 9);
        assert(uuid_set->intervals->next->gno_end == 9);
        assert(uuid_set->intervals->next->next == NULL);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);
        uuidSetFree(uuid_set);
        return 1;
    }

    int test_uuidSetEstimatedEncodeBufferSize() {
        uuidSet* uuid_set = uuidSetNew("A", 1, 1);
        assert(uuidSetEstimatedEncodeBufferSize(uuid_set) > 3);
        uuidSetAdd(uuid_set, 3);
        assert(uuidSetEstimatedEncodeBufferSize(uuid_set) > 5);
        uuidSetAdd(uuid_set, 5);
        assert(uuidSetEstimatedEncodeBufferSize(uuid_set) > 7);
        uuidSetFree(uuid_set);

        char* decode_str = "A:1:3:5:7:9";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        size_t max_len = uuidSetEstimatedEncodeBufferSize(uuid_set);
        assert(max_len > strlen(decode_str));
        uuidSetFree(uuid_set);
        return 1;
    }

    int test_uuidSetEncode() {
        char *decode_str = "A:1:2:3:4:5";
        uuidSet* uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        size_t max_len = uuidSetEstimatedEncodeBufferSize(uuid_set);
        char uuid_set_str[max_len];
        size_t len = uuidSetEncode(uuid_set, uuid_set_str);
        uuid_set_str[len] = '\0';
        assert(strcmp(uuid_set_str, decode_str) == 0);
        uuidSetFree(uuid_set);
        return 1;
    }

    int test_uuidSetAddGtidInterval() {
        //(0) + (1-2) = (1-2)
        char* decode_str;
        char buf[100];
        size_t buf_len;
        uuidSet* uuid_set;
        gtidInterval* interval;
        decode_str = "A:0";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(1,2);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-2"));
        assert(strncmp(buf, "A:1-2", buf_len) == 0);

        //  A  B  C
        // in A 
        //(4-5,7-8,10-11) + (1-2) = (1-2,4,-5,7-8,10-11) 
        decode_str = "A:4-5:7-8:10-11";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(1,2);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-2:4-5:7-8:10-11"));
        assert(strncmp(buf, "A:1-2:4-5:7-8:10-11", buf_len) == 0);

        //  A {gno_start} B {gno_end} C
        // in B
        //(1-5:7-8:10-11) + (2-3) = (1-5:7-8:10-11)
        decode_str = "A:1-5:7-8:10-11";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(2,3);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 0);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-5:7-8:10-11"));
        assert(strncmp(buf, "A:1-5:7-8:10-11", buf_len) == 0);

        //  A {gno_start} B {gno_end} C 
        // in C
        // (1-2:7-8:10-11) + (4-5) = (1-2:4-5:7-8:10-11)
        decode_str = "A:1-2:7-8:10-11";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(4,5);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-2:4-5:7-8:10-11"));
        assert(strncmp(buf, "A:1-2:4-5:7-8:10-11", buf_len) == 0);

        //  A {gno_start} B {gno_end} C
        // in A + B
        decode_str = "A:3-5:7-8:10-11";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(1,4);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-5:7-8:10-11"));
        assert(strncmp(buf, "A:1-5:7-8:10-11", buf_len) == 0);

        //  A {gno_start} B {gno_end} C
        // in B + C
        decode_str = "A:1-3:7-8:10-11";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(2,5);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-5:7-8:10-11"));
        assert(strncmp(buf, "A:1-5:7-8:10-11", buf_len) == 0);

        //  A {gno_start} B {gno_end} C
        // in A + C
        decode_str = "A:2-3:7-8:10-11";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(1,5);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-5:7-8:10-11"));
        assert(strncmp(buf, "A:1-5:7-8:10-11", buf_len) == 0);

        // A {gno_start} B {gno_end} C {all_next_start} D {all_next_end} E
        // A + D
        //(A:2-3:6-8:11-13) + (A:1-7) = (A:1-8:11-13)
        decode_str = "A:2-3:6-8:11-13";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(1,7);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-8:11-13"));
        assert(strncmp(buf, "A:1-8:11-13", buf_len) == 0);

        //(A:2-3:6-8:11-13:15-20) + (A:1-11) = (A:1-13:15-20)
        decode_str = "A:2-3:6-8:11-13";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(1,7);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-8:11-13"));
        assert(strncmp(buf, "A:1-8:11-13", buf_len) == 0);

        //(A:2-3:6-8:11-13) + (A:1-12) = (A:1-13)
        decode_str = "A:2-3:6-8:11-13";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(1,12);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);      
        assert(buf_len == strlen("A:1-13"));
        assert(strncmp(buf, "A:1-13", buf_len) == 0);

        // B + D 
        //(3-5:7-8:10-11) + (1-4) = (1-5:7-8:10-11)
        decode_str = "A:3-5:7-9:11-12";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(4,8);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:3-9:11-12"));
        assert(strncmp(buf, "A:3-9:11-12", buf_len) == 0);

        //(3-5:7-8:10-11) + (A:4-11) = (A:3-12)
        decode_str = "A:3-5:7-9:11-12";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(4,11);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);

        assert(buf_len == strlen("A:3-12"));
        assert(strncmp(buf, "A:3-12", buf_len) == 0);

        //C + D
        //(A:2-3:7-9:11-12) + (A:5-8)=(A:2-3:5-9:11-12)
        decode_str = "A:2-3:7-9:11-12";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(5,8);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-3:5-9:11-12"));
        assert(strncmp(buf, "A:2-3:5-9:11-12", buf_len) == 0);

        //(A:2-3:7-9:11-12:15-16) + (A:5-11)=(A:2-3:5-12:15-16)
        decode_str = "A:2-3:7-9:11-12:15-16";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(5,11);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-3:5-12:15-16"));
        assert(strncmp(buf, "A:2-3:5-12:15-16", buf_len) == 0);

        //D
        decode_str = "A:2-3:7-9:11-12";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(5,8);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-3:5-9:11-12"));
        assert(strncmp(buf, "A:2-3:5-9:11-12", buf_len) == 0);
       
        decode_str = "A:2-3:7-9:11-19";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(13,16);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 0);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-3:7-9:11-19"));
        assert(strncmp(buf, "A:2-3:7-9:11-19", buf_len) == 0);
         
        //A + E 
        decode_str = "A:2-3:7-9:11-12";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(1,14);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-14"));
        assert(strncmp(buf, "A:1-14", buf_len) == 0);
        

        decode_str = "A:2-3:7-9:12-13:15-19";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(1,13);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-13:15-19"));
        assert(strncmp(buf, "A:1-13:15-19", buf_len) == 0);

        //B+E
        //(A:2-4:7-9:12-13:15-19) + (A:3-20) = (A:2-20)
        decode_str = "A:2-4:7-9:12-13:15-19";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(3,20);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-20"));
        assert(strncmp(buf, "A:2-20", buf_len) == 0);
        

        //(A:2-4:7-9:11-13:15-19) + (A:3-12) = (A:2-13:15-19)
        decode_str = "A:2-4:7-9:11-13:15-19";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(3,12);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-13:15-19"));
        assert(strncmp(buf, "A:2-13:15-19", buf_len) == 0);

        //C+E
        //(A:2-4:7-9:11-13:15-19) + (A:6-20) = (A:2-4:6-20)
        decode_str = "A:2-4:7-9:11-13:15-19";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(6,20);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-4:6-20"));
        assert(strncmp(buf, "A:2-4:6-20", buf_len) == 0);

        //(A:2-4:7-9:11-13:16-19) + (A:6-14) = (A:2-4:6-14:16-19)
        decode_str = "A:2-4:7-9:11-13:16-19";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(6,14);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-4:6-14:16-19"));
        assert(strncmp(buf, "A:2-4:6-14:16-19", buf_len) == 0);
        //D+E
        //(A:2-4:7-9:11-13:15-19) + (A:8-20) = (A:2-4:7-20)
        decode_str = "A:2-4:7-9:11-13:15-19";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(8,20);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-4:7-20"));
        assert(strncmp(buf, "A:2-4:7-20", buf_len) == 0);

        //(A:2-4:7-9:11-13:15-19) + (A:8-12) = (A:2-4:7-13:15-19) 
        decode_str = "A:2-4:7-9:11-13:15-19";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(8,12);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-4:7-13:15-19"));
        assert(strncmp(buf, "A:2-4:7-13:15-19", buf_len) == 0);

        //E 
        decode_str = "A:2-3:7-9:11-12";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(14,20);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-3:7-9:11-12:14-20"));
        assert(strncmp(buf, "A:2-3:7-9:11-12:14-20", buf_len) == 0);     

        //(A:2-3:7-8:11-12) + (A:10-13) = (A:2-3:7-8:10-13)
        decode_str = "A:2-3:7-8:11-12";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(10,13);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-3:7-8:10-13"));
        assert(strncmp(buf, "A:2-3:7-8:10-13", buf_len) == 0);   
        
        decode_str = "A:1:3";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(2,2);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-3"));
        assert(strncmp(buf, "A:1-3", buf_len) == 0);   
        
        decode_str = "A:1:4";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(2,3);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-4"));
        assert(strncmp(buf, "A:1-4", buf_len) == 0);   
        
        decode_str = "A:1:4";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(2,3);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:1-4"));
        assert(strncmp(buf, "A:1-4", buf_len) == 0);   
        
        decode_str = "A:4-5";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(2,3);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:2-5"));
        assert(strncmp(buf, "A:2-5", buf_len) == 0);   

        decode_str = "A:4-5";
        uuid_set = uuidSetDecode(decode_str, strlen(decode_str));
        interval = gtidIntervalNewRange(6,7);
        assert(uuidSetAddGtidInterval(uuid_set, interval) == 1);
        buf_len = uuidSetEncode(uuid_set, buf);
        assert(buf_len == strlen("A:4-7"));
        assert(strncmp(buf, "A:4-7", buf_len) == 0);  
  
        return 1;
    }


    int test_uuidSetAdd() {
        uuidSet* uuid_set = uuidSetNew("A", 1, 1);
        uuidSetAdd(uuid_set, 3);
        assert(uuid_set->intervals->next->gno_start == 3);
        assert(uuid_set->intervals->next->gno_end == 3);
  
        char buf[100];
        size_t len = uuidSetEncode(uuid_set, buf);
        assert(len == 5); //A:1:3
        assert(strncmp(buf, "A:1:3", 5) == 0); 

        uuidSetFree(uuid_set);


        uuid_set = uuidSetNew("A", 1, 5);
        uuidSetAdd(uuid_set, 6);
        uuidSetAdd(uuid_set, 8);
        uuidSetAdd(uuid_set, 9);
        //add 9 to 5-6,8-9
        assert(uuidSetAdd(uuid_set, 9) == 0);
        uuidSetFree(uuid_set);

        uuid_set = uuidSetNew("A", 1, 1);
        uuidSetAdd(uuid_set, 5);
        uuidSetAdd(uuid_set, 6);
        uuidSetAdd(uuid_set, 11);
        uuidSetAdd(uuid_set, 13);
        uuidSetAdd(uuid_set, 20);
        uuidSetAdd(uuid_set, 19);
        uuidSetAdd(uuid_set, 1);
        uuidSetAdd(uuid_set, 12);
        uuidSetAdd(uuid_set, 3);
        uuidSetAdd(uuid_set, 13);
        uuidSetAdd(uuid_set, 13);
        uuidSetAdd(uuid_set, 14);
        uuidSetAdd(uuid_set, 12);
        //Manual created case: result should be A:1:3:5-6:11-14:19-20
        assert(uuid_set->intervals->gno_start == 1 );
        assert(uuid_set->intervals->gno_end == 1);
        assert(uuid_set->intervals->next->gno_start == 3);
        assert(uuid_set->intervals->next->gno_end == 3);
        assert(uuid_set->intervals->next->next->gno_start == 5);
        assert(uuid_set->intervals->next->next->gno_end == 6);
        assert(uuid_set->intervals->next->next->next->gno_start == 11);
        assert(uuid_set->intervals->next->next->next->gno_end == 14);
        assert(uuid_set->intervals->next->next->next->next->gno_start == 19);
        assert(uuid_set->intervals->next->next->next->next->gno_end == 20);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);
        
        uuidSetFree(uuid_set);

        uuid_set = uuidSetNew("A", 1, 9);
        uuidSetAdd(uuid_set, 8);
        //Add 8 to 9
        assert(uuid_set->intervals->gno_start == 8);
        assert(uuid_set->intervals->gno_end == 9);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);

        uuidSetAdd(uuid_set, 6);
        //Add 6 to 8-9
        assert(uuid_set->intervals->gno_start == 6);
        assert(uuid_set->intervals->gno_end == 6);
        assert(uuid_set->intervals->next->gno_start == 8);
        assert(uuid_set->intervals->next->gno_end == 9);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);

        //"Add 8 to 8-9"
        assert(uuidSetAdd(uuid_set, 8) == 0);

        uuidSetAdd(uuid_set, 7);
        //"Add 7 to 6,8-9"
        assert(
            uuid_set->intervals->gno_start == 6);
        assert(uuid_set->intervals->gno_end == 9);
        assert(uuid_set->intervals->next == NULL);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);

        uuidSetAdd(uuid_set, 100);
        //"Add 100 to 6-9"
        assert(uuid_set->intervals->gno_start == 6);
        assert(uuid_set->intervals->gno_end == 9);
        assert(uuid_set->intervals->next->gno_start == 100);
        assert(uuid_set->intervals->next->gno_end == 100);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);

        uuidSetFree(uuid_set);


        uuid_set = uuidSetNew("ABC",1, 9);

        //Create an new uuid set with 9
        assert(uuid_set->intervals->gno_start == 9);
        assert(uuid_set->intervals->gno_end == 9);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);

        uuidSetAdd(uuid_set, 7);
        //"Add 7 to 9"
        assert(uuid_set->intervals->gno_start == 7);
        assert(uuid_set->intervals->gno_end == 7);
        assert(uuid_set->intervals->next->gno_start == 9);
        assert(uuid_set->intervals->next->gno_end == 9);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);

        uuidSetFree(uuid_set);
        return 1;
    }

    int test_uuidSetRaise() {
        uuidSet* uuid_set = uuidSetNew("A", 1, 5);
        uuidSetRaise(uuid_set, 1);
        char buf[100];
        size_t len = uuidSetEncode(uuid_set, buf);
        assert(len == 5); //A:1:5
        assert(strncmp(buf, "A:1:5", len) == 0); 
        uuidSetFree(uuid_set);

        uuid_set = uuidSetNew("A", 1, 5);
        uuidSetRaise(uuid_set, 6);
        assert(uuid_set->intervals->gno_start == 1 );
        assert(uuid_set->intervals->gno_end == 6);
        assert(uuid_set->intervals->next == NULL);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);
        uuidSetFree(uuid_set);
        uuid_set = uuidSetNew("A", 1, 5);
        uuidSetAdd(uuid_set, 7);
        uuidSetRaise(uuid_set, 6);
        assert(uuid_set->intervals->gno_start == 1);
        assert(uuid_set->intervals->gno_end == 7);
        assert(uuid_set->intervals->next == NULL);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);

        uuid_set = uuidSetNew("A", 1, 5);
        uuidSetRaise(uuid_set, 3);
        assert(uuid_set->intervals->gno_start == 1);
        assert(uuid_set->intervals->gno_end == 3);
        assert(uuid_set->intervals->next->gno_start == 5);
        assert(uuid_set->intervals->next->gno_end == 5);
        assert(uuid_set->intervals->next->next == NULL);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);
        uuidSetFree(uuid_set);
        
        char* uuidset = "A:1:3:5-6:11-14:19-20";
        uuid_set = uuidSetDecode(uuidset, strlen(uuidset));
        // uuidset = uuidSetEncode(uuid_set, uuidset);
        //Manual created case (encode): result should be A:1:3:5-6:1-14:19-20
        // assert(strcmp(uuidset, "A:1:3:5-6:11-14:19-20") == 0);
        uuidSetRaise(uuid_set, 30);
        //Manual created case (raise to 30)
        assert(uuid_set->intervals->gno_start == 1);
        assert(uuid_set->intervals->gno_end == 30);
        assert(uuid_set->intervals->next == NULL);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);

        uuidSetFree(uuid_set);
        return 1;
    }

    int test_uuidSetContains() {
        uuidSet* uuid_set = uuidSetNewRange("A", 1, 1, 5);
        assert(uuidSetContains(uuid_set, 1) == 1);
        assert(uuidSetContains(uuid_set, 6) == 0);

        uuid_set = uuidSetNew("A",1,5);
        uuidSetAdd(uuid_set, 8);
        uuidSetRaise(uuid_set, 6);
        assert(uuid_set->intervals->gno_start == 1);
        assert(uuid_set->intervals->gno_end == 6);
        assert(uuid_set->intervals->next->gno_start == 8);
        assert(uuid_set->intervals->next->gno_end == 8);
        assert(uuid_set->intervals->next->next == NULL);
        assert(memcmp(uuid_set->rpl_sid, "A\0", 2) == 0);


        //next of A:1-6:8 is 7
        assert(uuidSetNext(uuid_set, 0) == 7);
        //1 is in A:1-8
        assert(uuidSetContains(uuid_set, 1) == 1);
        //3 is in A:1-8
        assert(uuidSetContains(uuid_set, 3) == 1);
        //6 is in A:1-8
        assert(uuidSetContains(uuid_set, 6) == 1);
        //7 is not in A:1-8
        assert(uuidSetContains(uuid_set, 7) == 0);
        //8 is in A:1-8
        assert(uuidSetContains(uuid_set, 8) == 1);
        //30 is not in A:1-8
        assert(uuidSetContains(uuid_set, 30) == 0);

        uuidSetFree(uuid_set);

        return 1;
    }

    int test_uuidSetNext() {
        uuidSet* uuid_set = uuidSetNewRange("A", 1, 1, 5);
        assert(6 == uuidSetNext(uuid_set, 1));
        uuidSetRaise(uuid_set, 8);
        assert(9 == uuidSetNext(uuid_set, 1));
        uuidSetFree(uuid_set);

        uuid_set = uuidSetNew("A", 1, 5);
        uuidSetAdd(uuid_set, 6);
        //next of A:5-6 is 1
        assert(uuidSetNext(uuid_set, 0) == 1);
        //add 6 to 5-6 , return 0
        assert(uuidSetAdd(uuid_set, 6) == 0);

        uuidSetNext(uuid_set, 1);

        char uuidset[100];
        int uuidset_len = uuidSetEncode(uuid_set, uuidset);
        uuidset[uuidset_len] = '\0';
        //update next of A:5-6, will be A:1:5-6
        assert(strcmp(uuidset, "A:1:5-6") == 0);

        uuidSetNext(uuid_set, 1);
        uuidset_len = uuidSetEncode(uuid_set, uuidset);
        uuidset[uuidset_len] = '\0';
        //update next of A:1:5-6, will be A:1-2:5-6
        assert(strcmp(uuidset, "A:1-2:5-6") == 0);

        uuidSetNext(uuid_set, 1);
        uuidSetNext(uuid_set, 1);
        uuidSetNext(uuid_set, 1);
        uuidset_len = uuidSetEncode(uuid_set, uuidset);
        uuidset[uuidset_len] = '\0';
        //"update next 3 times of A:1-2:5-6, will be A:1-7"
        assert(strcmp(uuidset, "A:1-7") == 0);
        uuidSetFree(uuid_set);        
        return 1;
    }

    int test_uuidSetNextEncode() {
        uuidSet* uuid_set = uuidSetNewRange("A", 1, 1, 5);
        char buf[100];
        size_t len = uuidSetNextEncode(uuid_set, 1, buf);
        assert(3 == len);
        assert(strncmp(buf, "A:6", len) == 0);
        uuidSetRaise(uuid_set, 8);
        len = uuidSetNextEncode(uuid_set, 1, buf);
        assert(3 == len);
        assert(strncmp(buf, "A:9", len) == 0);
        uuidSetFree(uuid_set);

        char *gtidset = "A:1-7,B:9:11-13:20";
        gtidSet* gtid_set = gtidSetDecode(gtidset, strlen(gtidset));
        uuidSet* B = gtidSetFindUuidSet(gtid_set, "B", 1);
        char next[100];
        int next_len = 0;
        next_len = uuidSetNextEncode(B, 1, next);
        //B next of A:1-7,B:9:11-13:20 & Update
        assert(strncmp("B:1", next, next_len) == 0);
        assert(uuidSetContains(B, 1));

        next_len = uuidSetNextEncode(B, 1, next);
        next[next_len] = '\0';
        //B next of A:1-7,B:1:9:11-13:20 & Update
        assert(strcmp("B:2", next) == 0);
        assert(uuidSetContains(B, 2));

        next_len = uuidSetNextEncode(B, 1, next);
        next_len = uuidSetNextEncode(B, 1, next);
        next_len = uuidSetNextEncode(B, 1, next);
        next_len = uuidSetNextEncode(B, 1, next);
        next_len = uuidSetNextEncode(B, 1, next);
        next[next_len] = '\0';
        long long gno = 0;
        int rpl_sid_len = 0;
        uuidDecode(next, next_len, &gno, &rpl_sid_len);
        //B next of A:1-7,B:1-2:9:11-13:20 5 times & Update
        assert(strcmp("B:7", next) == 0);
        assert(gno == 7);
        assert(uuidSetContains(B, gno));

        next_len = uuidSetNextEncode(B, 1, next);
        //B next of A:1-7,B:1-7:9:11-13:20 & Update
        assert(strcmp("B:8", next) == 0);
        assert(uuidSetContains(B, gno));

        next_len = uuidSetNextEncode(B, 1, next);
        //"B next of A:1-7,B:1-9:11-13:20 & Update"
        assert(strcmp("B:10", next) == 0);
        assert(uuidSetContains(B, gno));

        next_len = uuidSetNextEncode(B, 1, next);
        //B next of A:1-7,B:1-13:20 & Update
        assert(strcmp("B:14", next) == 0);
        assert(uuidSetContains(B, 14));
        return 1;
    }

    int test_gtidSetNew() {
        gtidSet* gtid_set = gtidSetNew();
        assert(gtid_set->uuid_sets == NULL);
        assert(gtid_set->tail == NULL);
        return 1;
    }

    int test_gtidSetDecode() {
        char* gtid_set_str = "A:1,B:1";
        gtidSet* gtid_set = gtidSetDecode(gtid_set_str, 7);
        assert(strcmp(gtid_set->uuid_sets->rpl_sid, "A") == 0);
        assert(gtid_set->uuid_sets->intervals->gno_start == 1);
        assert(gtid_set->uuid_sets->intervals->gno_end == 1);
        assert(strcmp(gtid_set->uuid_sets->next->rpl_sid, "B") == 0);
        assert(gtid_set->uuid_sets->next->intervals->gno_start == 1);
        assert(gtid_set->uuid_sets->next->intervals->gno_end == 1);
        gtidSetFree(gtid_set);

        gtid_set_str = "A:1-7,B:9:11-13:20ABC";
        gtid_set = gtidSetDecode(gtid_set_str, strlen(gtid_set_str)-3);
        char gtid_str[100];
        int len = gtidSetEncode(gtid_set, gtid_str);
        //encode & decode A:1-7,B:9:11-13:20 string len equal
        assert(len == strlen(gtid_set_str) - 3);
        return 1;
    }

    int test_gtidSetEstimatedEncodeBufferSize() {

        //about max 
        char gtid_set_str[2000] = "";
        for(int i = 0; i < 1000; i++) {
            gtid_set_str[i] = 'A';
        }
        gtid_set_str[1000] = ':';
        size_t len = ll2string(gtid_set_str + 1001, 1000, __LONG_LONG_MAX__ );
        gtid_set_str[1001 + len] = '-';
        len += ll2string(gtid_set_str + 1002 + len, 1000, __LONG_LONG_MAX__ );
        gtid_set_str[1002 + len] = '\0';
        gtidSet* gtid_set = gtidSetDecode(gtid_set_str, strlen(gtid_set_str));
        size_t max_len = gtidSetEstimatedEncodeBufferSize(gtid_set);
        assert(max_len > strlen(gtid_set_str));

        //about empty
        gtid_set = gtidSetNew();
        max_len = gtidSetEstimatedEncodeBufferSize(gtid_set);
        //"estimated empty gtid set to string len > 0"
        assert(max_len > 0);
        
        return 1;
    }

    int test_gtidSetEncode() {
        gtidSet* gtid_set = gtidSetNew();
        char gtid_str[100];
        size_t len = gtidSetEncode(gtid_set, gtid_str);
        //empty encode len == 0
        assert(len == 0);

        gtidSetAdd(gtid_set, "A", 1, 1);
        len = gtidSetEncode(gtid_set, gtid_str);
        assert(len == 3);
        assert(strncmp(gtid_str, "A:1", len) == 0);
        gtidSetAdd(gtid_set, "B", 1, 1);
        len = gtidSetEncode(gtid_set, gtid_str);
        assert(len == 7);
        assert(strncmp(gtid_str, "A:1,B:1", len) == 0);
        gtidSetFree(gtid_set);


        char* gtid_set_str = "A:1-7,B:9:11-13:20";
        gtid_set = gtidSetDecode(gtid_set_str, strlen(gtid_set_str));
        len = gtidSetEncode(gtid_set, gtid_str);
        //encode & decode A:1-7,B:9:11-13:20 string len equal
        assert(len == strlen(gtid_set_str));
        //encode & decode A:1-7,B:9:11-13:20
        assert(strncmp(gtid_str, gtid_set_str, len) == 0);

        gtidSetFree(gtid_set);
        return 1;
    }

    int test_gtidSetFindUuidSet() {
        gtidSet* gtid_set = gtidSetDecode("A:1,B:2", 7);
        uuidSet* A = gtidSetFindUuidSet(gtid_set, "A", 1);
        assert(A != NULL);
        assert(strcmp(A->rpl_sid, "A") == 0);
        assert(A->intervals->gno_start == 1);
        uuidSet* B = gtidSetFindUuidSet(gtid_set, "B", 1);
        assert(B != NULL);
        assert(strcmp(B->rpl_sid, "B") == 0);
        assert(B->intervals->gno_start == 2);
        return 1;
    }

    int test_gtidSetAdd() {
        gtidSet* gtid_set = gtidSetNew();
        gtidSetAdd(gtid_set, "A", 1, 1);
        assert(strcmp(gtid_set->uuid_sets->rpl_sid, "A") == 0);
        assert(gtid_set->uuid_sets->intervals->gno_start == 1);
        assert(gtid_set->uuid_sets->intervals->gno_end == 1);
        gtidSetAdd(gtid_set, "A", 1, 2);
        assert(gtid_set->uuid_sets->intervals->gno_start == 1);
        assert(gtid_set->uuid_sets->intervals->gno_end == 2);

        gtidSetAdd(gtid_set, "B", 1, 1);
        assert(strcmp(gtid_set->uuid_sets->next->rpl_sid, "B") == 0);
        assert(gtid_set->uuid_sets->next->intervals->gno_start == 1);
        assert(gtid_set->uuid_sets->next->intervals->gno_end == 1);
        gtidSetFree(gtid_set);

        gtid_set = gtidSetNew();
        gtidSetAdd(gtid_set, "A", 1, 1);
        gtidSetAdd(gtid_set, "A", 1, 2);
        gtidSetAdd(gtid_set, "B", 1, 3);
        //Add A:1 A:2 B:3 to empty gtid set
        assert(memcmp(gtid_set->uuid_sets->rpl_sid, "A\0", 1) == 0);
        assert(gtid_set->uuid_sets->intervals->gno_start == 1);
        assert(gtid_set->uuid_sets->intervals->gno_end == 2);
        assert(memcmp(gtid_set->uuid_sets->next->rpl_sid, "B\0", 1) == 0);
        assert(gtid_set->uuid_sets->next->intervals->gno_start == 3);
        assert(gtid_set->uuid_sets->next->intervals->gno_end == 3);

        char gtid_str[gtidSetEstimatedEncodeBufferSize(gtid_set)];
        int len = gtidSetEncode(gtid_set, gtid_str);
        gtid_str[len] = '\0';
        //Add A:1 A:2 B:3 to empty gtid set (encode)
        assert(strcmp(gtid_str, "A:1-2,B:3") == 0);

        return 1;
    }

    int test_uuidDecode() {
        long long gno = 0;
        int uuid_index = 0;
        char* uuid = uuidDecode("ABCD:1", 6, &gno, &uuid_index);
        assert(uuid_index == 4);
        assert(strncmp(uuid, "ABCD", uuid_index) == 0);
        assert(gno == 1);
        return 1;
    }

    int test_gtidSetRaise() {
        char* gtid_set_str = "A:1:3:5:7";
        gtidSet* gtid_set = gtidSetDecode(gtid_set_str, strlen(gtid_set_str));
        gtidSetRaise(gtid_set, "A", 1, 10);
        assert(gtid_set->uuid_sets->intervals->gno_start == 1);
        assert(gtid_set->uuid_sets->intervals->gno_end == 10);
        gtidSetFree(gtid_set);
        gtid_set = gtidSetNew();
        gtidSetRaise(gtid_set, "A", 1, 1);
        assert(strcmp(gtid_set->uuid_sets->rpl_sid, "A") == 0);
        assert(gtid_set->uuid_sets->intervals->gno_start == 1);
        assert(gtid_set->uuid_sets->intervals->gno_end == 1);

        gtid_set_str = "A:1-2,B:3";
        gtid_set = gtidSetDecode(gtid_set_str, strlen(gtid_set_str));
        gtidSetAdd(gtid_set, "B", 1, 7);
        gtidSetRaise(gtid_set, "A", 1, 5);
        gtidSetRaise(gtid_set, "B", 1, 5);
        gtidSetRaise(gtid_set, "C", 1, 10);
        char gtid_str[100];
        int len = gtidSetEncode(gtid_set, gtid_str);
        gtid_str[len] = '\0';
        //Raise A & B to 5, C to 10, towards C:1-10,B:3:7,A:1-2
        assert(strcmp(gtid_str, "A:1-5,B:1-5:7,C:1-10") == 0);
        gtidSetFree(gtid_set);

        /* raise 0*/
        gtid_set = gtidSetNew();
        gtidSetAdd(gtid_set, "A", 1, 0);
        len = gtidSetEncode(gtid_set, gtid_str);
        gtid_str[len] = '\0';
        assert(strcmp(gtid_str, "A:0") == 0);
        gtidSetRaise(gtid_set, "A", 1, 0);
        len = gtidSetEncode(gtid_set, gtid_str);
        gtid_str[len] = '\0';
        assert(strcmp(gtid_str, "A:0") == 0);

        return 1;
    }

    int test_gtidSetAppendGtidSet() {
        char gtid_str[100];
        int str_len;
        char* A_str,*B_str;
        gtidSet *A,*B;
        
        
        //null + (B:1:3:5:7) = (B:1:3:5:7)
        A_str = "";
        A = gtidSetDecode(A_str, strlen(A_str));
        B_str = "B:1:3:5:7";
        B = gtidSetDecode(B_str, strlen(B_str));
        gtidSetAppendGtidSet(A, B);
        str_len = gtidSetEncode(A, gtid_str);
        assert(str_len == strlen("B:1:3:5:7"));
        assert(strncmp(gtid_str, "B:1:3:5:7",str_len) == 0);

        //(B:1:3:5:7) + null = (B:1:3:5:7)
        A_str = "B:1:3:5:7";
        A = gtidSetDecode(A_str, strlen(A_str));
        B_str = "";
        B = gtidSetDecode(B_str, strlen(B_str));
        gtidSetAppendGtidSet(A, B);
        str_len = gtidSetEncode(A, gtid_str);
        assert(str_len == strlen("B:1:3:5:7"));
        assert(strncmp(gtid_str, "B:1:3:5:7",str_len) == 0);

        //(A:1:3:5:7) + (B:1:3:5:7) = (A:1:3:5:7,B:1:3:5:7)
        A_str = "A:1:3:5:7";
        A = gtidSetDecode(A_str, strlen(A_str));
        B_str = "B:1:3:5:7";
        B = gtidSetDecode(B_str, strlen(B_str));
        gtidSetAppendGtidSet(A, B);
        str_len = gtidSetEncode(A, gtid_str);
        assert(str_len == strlen("A:1:3:5:7,B:1:3:5:7"));
        assert(strncmp(gtid_str, "A:1:3:5:7,B:1:3:5:7",str_len) == 0);
        str_len = gtidSetEncode(B, gtid_str);
        assert(str_len == strlen("B:1:3:5:7"));
        assert(strncmp(gtid_str, "B:1:3:5:7",str_len) == 0);
        

        //(A:1:3:5:7) + (B:1:3:5:7,C:1:3:5:7) = (A:1:3:5:7,B:1:3:5:7,C:1:3:5:7)
        A_str = "A:1:3:5:7";
        A = gtidSetDecode(A_str, strlen(A_str));
        B_str = "B:1:3:5:7,C:1:3:5:7";
        B = gtidSetDecode(B_str, strlen(B_str));
        gtidSetAppendGtidSet(A, B);

        str_len = gtidSetEncode(A, gtid_str);
        assert(str_len == strlen("A:1:3:5:7,B:1:3:5:7,C:1:3:5:7"));
        assert(strncmp(gtid_str, "A:1:3:5:7,B:1:3:5:7,C:1:3:5:7",str_len) == 0);

        //(A:1:3:5:7) + (A:2:4:6:8) = (B:1-8)
        A_str = "A:1:3:5:7";
        A = gtidSetDecode(A_str, strlen(A_str));
        B_str = "A:2:4:6:8";
        B = gtidSetDecode(B_str, strlen(B_str));
        str_len = gtidSetEncode(A, gtid_str);
        assert(str_len == strlen("A:1-8"));
        assert(strncmp(gtid_str, "A:1-8",str_len) == 0);

        return 1;
    }

    int test_api(void) {
        {
            #ifdef CTRIP_GTID_TEST
                test_cond("writeBuf function",
                    test_writeBuf() == 1);
                test_cond("stringNew function",
                    test_stringNew() == 1);
                test_cond("gtidSetAppendUuidSet function",
                    test_gtidSetAppendUuidSet() == 1);
                test_cond("gtidSetRaise function", 
                    test_gtidSetRaise() == 1);
            #endif
            test_cond("gtidIntervalNew function",
                test_gtidIntervalNew() == 1);
            test_cond("gtidIntervalNewRange function",
                test_gtidIntervalNewRange() == 1);
            test_cond("gtidIntervalDump function",
                test_gtidIntervalDump() == 1);
            test_cond("gtidIntervalDecode function",
                test_gtidIntervalDecode() == 1);
            test_cond("gtidIntervalEncode function",
                test_gtidIntervalEncode() == 1);
            test_cond("uuidSetNew function",
                test_uuidSetNew() == 1);
            test_cond("uuidSetNewRange function",
                test_uuidSetNewRange() == 1);
            test_cond("uuidSetDump function",
                test_uuidSetDump() == 1);
            test_cond("uuidSetDecode function",
                test_uuidSetDecode() == 1);
            test_cond("uuidSetEstimatedEncodeBufferSize function",
                test_uuidSetEstimatedEncodeBufferSize() == 1);
            test_cond("uuidSetEncode function", 
                test_uuidSetEncode() == 1);
            test_cond("uuidSetAddGtidInterval function",
                test_uuidSetAddGtidInterval() == 1);
            test_cond("uuidSetAdd function", 
                test_uuidSetAdd() == 1);
            test_cond("uuidSetRaise function", 
                test_uuidSetRaise() == 1);
            test_cond("uuidSetContains function", 
                test_uuidSetContains() == 1);
            test_cond("uuidSetNext function",
                test_uuidSetNext() == 1);
            test_cond("uuidSetNextEncode function",
                test_uuidSetNextEncode() == 1);
            test_cond("gtidSetNew function",
                test_gtidSetNew() == 1);
            test_cond("gtidSetDecode function", 
                test_gtidSetDecode() == 1);
            test_cond("gtidSetEstimatedEncodeBufferSize function",
                test_gtidSetEstimatedEncodeBufferSize() == 1);
            test_cond("gtidSetEncode function",
                test_gtidSetEncode() == 1);
            test_cond("gtidSetAdd function",
                test_gtidSetAdd() == 1);
            test_cond("gtidSetFindUuidSet function",
                test_gtidSetFindUuidSet() == 1);
            test_cond("uuidDecode function",
                test_uuidDecode() == 1);
            // test_cond("gtidSetAppendGtidSet function",
            //     test_gtidSetAppendGtidSet() == 1);

        } test_report()
        return 1;
    }

    



int main(void) {
    
    test_api();
    return 0;
}