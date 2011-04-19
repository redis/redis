#ifndef __HIREDIS_FMACRO_H
#define __HIREDIS_FMACRO_H

#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#endif

#ifdef __linux__
#define _XOPEN_SOURCE 700
#else
#define _XOPEN_SOURCE
#endif

#endif
