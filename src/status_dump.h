// Create a new thread to dump status periodically.
#ifndef THIRD_PARTY_CLOUDREDISPRODUCTION_V3_2_11_SRC_STATUS_DUMP_H_
#define THIRD_PARTY_CLOUDREDISPRODUCTION_V3_2_11_SRC_STATUS_DUMP_H_

#define USEC_PER_SEC 1000000

#define MIN_STATUS_DUMP_INTERVAL_SEC 0
#define MAX_STATUS_DUMP_INTERVAL_SEC 3600

void reset_status_dump_thread(int new_status_dump_interval_sec);
void update_status_dump_interval(int new_status_dump_interval_sec);
int get_status_dump_interval_sec();


#endif  // THIRD_PARTY_CLOUDREDISPRODUCTION_V3_2_11_SRC_STATUS_DUMP_H_
