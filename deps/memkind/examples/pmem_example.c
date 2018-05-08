/*
 * Copyright (c) 2015, 2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY LOG OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <memkind.h>
#include <memkind/internal/memkind_pmem.h>

#include <sys/param.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define PMEM1_MAX_SIZE	(MEMKIND_PMEM_MIN_SIZE * 2)
#define PMEM2_MAX_SIZE	((size_t)1024 * 1024 * 1024 * 1024)

#define CHUNK_SIZE	(4 * 1024 * 1024) /* assume 4MB chunks */

static int pmem_tmpfile(const char *dir, size_t size, int *fd, void **addr)
{
    static char template[] = "/pmem.XXXXXX";
    int err = 0;
    int oerrno;

    char fullname[strlen(dir) + sizeof (template)];
    (void) strcpy(fullname, dir);
    (void) strcat(fullname, template);

    if ((*fd = mkstemp(fullname)) < 0) {
        perror("mkstemp()");
        err = MEMKIND_ERROR_RUNTIME;
        goto exit;
    }

    (void) unlink(fullname);

    if (ftruncate(*fd, size) != 0) {
        err = MEMKIND_ERROR_RUNTIME;
        goto exit;
    }

    *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    if (*addr == MAP_FAILED) {
        err = MEMKIND_ERROR_RUNTIME;
        goto exit;
    }

    return err;

exit:
    oerrno = errno;
    if (*fd != -1) {
        (void) close(*fd);
    }
    *fd = -1;
    *addr = NULL;
    errno = oerrno;
    return err;
}

int
main(int argc, char *argv[])
{
    struct memkind *pmem_kind1;
    int err = 0;
    int fd;
    void *addr;

    /* create PMEM partition */
    err = memkind_create_pmem(".", PMEM1_MAX_SIZE, &pmem_kind1);
    if (err) {
        perror("memkind_create_pmem()");
        fprintf(stderr, "Unable to create pmem partition\n");
        return errno ? -errno : 1;
    }

    /* alternate way to create PMEM partition */
    err = pmem_tmpfile(".", PMEM2_MAX_SIZE, &fd, &addr);
    if (err) {
        fprintf(stderr, "Unable to create temporary file\n");
        return errno ? -errno : 1;
    }

    const size_t size = 512;
    char *pmem_str10 = NULL;
    char *pmem_str11 = NULL;
    char *pmem_str12 = NULL;
    char *pmem_str = NULL;

    pmem_str10 = (char *)memkind_malloc(pmem_kind1, size);
    if (pmem_str10 == NULL) {
        perror("memkind_malloc()");
        fprintf(stderr, "Unable to allocate pmem string (pmem_str10)\n");
        return errno ? -errno : 1;
    }

    /* next chunk mapping */
    pmem_str11 = (char *)memkind_malloc(pmem_kind1, 8 * 1024 * 1024);
    if (pmem_str11 == NULL) {
        perror("memkind_malloc()");
        fprintf(stderr, "Unable to allocate pmem string (pmem_str11)\n");
        return errno ? -errno : 1;
    }

    /* extend the heap #1 */
    pmem_str12 = (char *)memkind_malloc(pmem_kind1, 16 * 1024 * 1024);
    if (pmem_str12 == NULL) {
        perror("memkind_malloc()");
        fprintf(stderr, "Unable to allocate pmem string (pmem_str12)\n");
        return errno ? -errno : 1;
    }

    /* OOM #1 */
    pmem_str = (char *)memkind_malloc(pmem_kind1, 16 * 1024 * 1024);
    if (pmem_str != NULL) {
        perror("memkind_malloc()");
        fprintf(stderr, "Failure, this allocation should not be possible (expected result was NULL)\n");
        return errno ? -errno : 1;
    }

    sprintf(pmem_str10, "Hello world from persistent memory1\n");

    fprintf(stdout, "%s", pmem_str10);

    memkind_free(pmem_kind1, pmem_str10);
    memkind_free(pmem_kind1, pmem_str11);
    memkind_free(pmem_kind1, pmem_str12);

    return 0;
}
