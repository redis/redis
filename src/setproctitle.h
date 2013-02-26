#ifndef _PROCTITLE_H_
#define _PROCTITLE_H_
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/prctl.h>

#include "sds.h"

#ifndef PR_SET_NAME
#define PR_SET_NAME 15
#endif

#ifndef PR_GET_NAME
#define PR_GET_NAME 16
#endif


void init_proc_title(int argc, char **argv);

void set_proc_title(const char *title);
void set_proc_name(const char *name);

void get_proc_name(char *name);

#endif // _PROCTITLE_H_
