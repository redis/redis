/* ==========================================================================
 * setproctitle.c - Linux/Darwin setproctitle.
 * --------------------------------------------------------------------------
 * Copyright (C) 2010  William Ahern
 * Copyright (C) 2013  Salvatore Sanfilippo
 * Copyright (C) 2013  Stam He
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
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>	/* NULL size_t */
#include <stdarg.h>	/* va_list va_start va_end */
#include <stdlib.h>	/* malloc(3) setenv(3) clearenv(3) setproctitle(3) getprogname(3) */
#include <stdio.h>	/* vsnprintf(3) snprintf(3) */

#include <string.h>	/* strlen(3) strchr(3) strdup(3) memset(3) memcpy(3) */

#include <errno.h>	/* errno program_invocation_name program_invocation_short_name */

#if !defined(HAVE_SETPROCTITLE)
#if (defined __NetBSD__ || defined __FreeBSD__ || defined __OpenBSD__ || defined __DragonFly__)
#define HAVE_SETPROCTITLE 1
#else
#define HAVE_SETPROCTITLE 0
#endif
#endif


#if !HAVE_SETPROCTITLE
#if (defined __linux || defined __APPLE__)

#ifdef __GLIBC__
#define HAVE_CLEARENV
#endif

extern char **environ;

static struct {
	/* original value */
	const char *arg0;

	/* title space available */
	char *base, *end;

	 /* pointer to original nul character within base */
	char *nul;

	_Bool reset;
	int error;
} SPT;


#ifndef SPT_MIN
#define SPT_MIN(a, b) (((a) < (b))? (a) : (b))
#endif

static inline size_t spt_min(size_t a, size_t b) {
	return SPT_MIN(a, b);
} /* spt_min() */


/*
 * For discussion on the portability of the various methods, see
 * http://lists.freebsd.org/pipermail/freebsd-stable/2008-June/043136.html
 */
int spt_clearenv(void) {
#ifdef HAVE_CLEARENV
	return clearenv();
#else
	extern char **environ;
	static char **tmp;

	if (!(tmp = malloc(sizeof *tmp)))
		return errno;

	tmp[0]  = NULL;
	environ = tmp;

	return 0;
#endif
} /* spt_clearenv() */


static int spt_copyenv(int envc, char *oldenv[]) {
	extern char **environ;
	char **envcopy = NULL;
	char *eq;
	int i, error;
	int envsize;

	if (environ != oldenv)
		return 0;

	/* Copy environ into envcopy before clearing it. Shallow copy is
	 * enough as clearenv() only clears the environ array.
	 */
	envsize = (envc + 1) * sizeof(char *);
	envcopy = malloc(envsize);
	if (!envcopy)
		return ENOMEM;
	memcpy(envcopy, oldenv, envsize);

	/* Note that the state after clearenv() failure is undefined, but we'll
	 * just assume an error means it was left unchanged.
	 */
	if ((error = spt_clearenv())) {
		environ = oldenv;
		free(envcopy);
		return error;
	}

	/* Set environ from envcopy */
	for (i = 0; envcopy[i]; i++) {
		if (!(eq = strchr(envcopy[i], '=')))
			continue;

		*eq = '\0';
		error = (0 != setenv(envcopy[i], eq + 1, 1))? errno : 0;
		*eq = '=';

		/* On error, do our best to restore state */
		if (error) {
#ifdef HAVE_CLEARENV
			/* We don't assume it is safe to free environ, so we
			 * may leak it. As clearenv() was shallow using envcopy
			 * here is safe.
			 */
			environ = envcopy;
#else
			free(envcopy);
			free(environ);  /* Safe to free, we have just alloc'd it */
			environ = oldenv;
#endif
			return error;
		}
	}

	free(envcopy);
	return 0;
} /* spt_copyenv() */


static int spt_copyargs(int argc, char *argv[]) {
	char *tmp;
	int i;

	for (i = 1; i < argc || (i >= argc && argv[i]); i++) {
		if (!argv[i])
			continue;

		if (!(tmp = strdup(argv[i])))
			return errno;

		argv[i] = tmp;
	}

	return 0;
} /* spt_copyargs() */

/* Initialize and populate SPT to allow a future setproctitle()
 * call.
 *
 * As setproctitle() basically needs to overwrite argv[0], we're
 * trying to determine what is the largest contiguous block
 * starting at argv[0] we can use for this purpose.
 *
 * As this range will overwrite some or all of the argv and environ
 * strings, a deep copy of these two arrays is performed.
 */
void spt_init(int argc, char *argv[]) {
        char **envp = environ;
	char *base, *end, *nul, *tmp;
	int i, error, envc;

	if (!(base = argv[0]))
		return;

	/* We start with end pointing at the end of argv[0] */
	nul = &base[strlen(base)];
	end = nul + 1;

	/* Attempt to extend end as far as we can, while making sure
	 * that the range between base and end is only allocated to
	 * argv, or anything that immediately follows argv (presumably
	 * envp).
	 */
	for (i = 0; i < argc || (i >= argc && argv[i]); i++) {
		if (!argv[i] || argv[i] < end)
			continue;

		if (end >= argv[i] && end <= argv[i] + strlen(argv[i]))
			end = argv[i] + strlen(argv[i]) + 1;
	}

	/* In case the envp array was not an immediate extension to argv,
	 * scan it explicitly.
	 */
	for (i = 0; envp[i]; i++) {
		if (envp[i] < end)
			continue;

		if (end >= envp[i] && end <= envp[i] + strlen(envp[i]))
			end = envp[i] + strlen(envp[i]) + 1;
	}
	envc = i;

	/* We're going to deep copy argv[], but argv[0] will still point to
	 * the old memory for the purpose of updating the title so we need
	 * to keep the original value elsewhere.
	 */
	if (!(SPT.arg0 = strdup(argv[0])))
		goto syerr;

#if __linux__
	if (!(tmp = strdup(program_invocation_name)))
		goto syerr;

	program_invocation_name = tmp;

	if (!(tmp = strdup(program_invocation_short_name)))
		goto syerr;

	program_invocation_short_name = tmp;
#elif __APPLE__
	if (!(tmp = strdup(getprogname())))
		goto syerr;

	setprogname(tmp);
#endif

    /* Now make a full deep copy of the environment and argv[] */
	if ((error = spt_copyenv(envc, envp)))
		goto error;

	if ((error = spt_copyargs(argc, argv)))
		goto error;

	SPT.nul  = nul;
	SPT.base = base;
	SPT.end  = end;

	return;
syerr:
	error = errno;
error:
	SPT.error = error;
} /* spt_init() */


#ifndef SPT_MAXTITLE
#define SPT_MAXTITLE 255
#endif

void setproctitle(const char *fmt, ...) {
	char buf[SPT_MAXTITLE + 1]; /* use buffer in case argv[0] is passed */
	va_list ap;
	char *nul;
	int len, error;

	if (!SPT.base)
		return;

	if (fmt) {
		va_start(ap, fmt);
		len = vsnprintf(buf, sizeof buf, fmt, ap);
		va_end(ap);
	} else {
		len = snprintf(buf, sizeof buf, "%s", SPT.arg0);
	}

	if (len <= 0)
		{ error = errno; goto error; }

	if (!SPT.reset) {
		memset(SPT.base, 0, SPT.end - SPT.base);
		SPT.reset = 1;
	} else {
		memset(SPT.base, 0, spt_min(sizeof buf, SPT.end - SPT.base));
	}

	len = spt_min(len, spt_min(sizeof buf, SPT.end - SPT.base) - 1);
	memcpy(SPT.base, buf, len);
	nul = &SPT.base[len];

	if (nul < SPT.nul) {
		*SPT.nul = '.';
	} else if (nul == SPT.nul && &nul[1] < SPT.end) {
		*SPT.nul = ' ';
		*++nul = '\0';
	}

	return;
error:
	SPT.error = error;
} /* setproctitle() */


#endif /* __linux || __APPLE__ */
#endif /* !HAVE_SETPROCTITLE */

#ifdef SETPROCTITLE_TEST_MAIN
int main(int argc, char *argv[]) {
	spt_init(argc, argv);

	printf("SPT.arg0: [%p] '%s'\n", SPT.arg0, SPT.arg0);
	printf("SPT.base: [%p] '%s'\n", SPT.base, SPT.base);
	printf("SPT.end: [%p] (%d bytes after base)'\n", SPT.end, (int) (SPT.end - SPT.base));
	return 0;
}
#endif
