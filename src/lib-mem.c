/*
 *  zzuf - general purpose fuzzer
 *  Copyright (c) 2006,2007 Sam Hocevar <sam@zoy.org>
 *                All Rights Reserved
 *
 *  $Id$
 *
 *  This program is free software. It comes without any warranty, to
 *  the extent permitted by applicable law. You can redistribute it
 *  and/or modify it under the terms of the Do What The Fuck You Want
 *  To Public License, Version 2, as published by Sam Hocevar. See
 *  http://sam.zoy.org/wtfpl/COPYING for more details.
 */

/*
 *  load-mem.c: loaded memory handling functions
 */

#include "config.h"

/* Need this for RTLD_NEXT */
#define _GNU_SOURCE
/* Use this to get mmap64() on glibc systems */
#define _LARGEFILE64_SOURCE
/* Use this to get posix_memalign */
#if defined HAVE_POSIX_MEMALIGN
#   define _XOPEN_SOURCE 600
#endif

#if defined HAVE_STDINT_H
#   include <stdint.h>
#elif defined HAVE_INTTYPES_H
#   include <inttypes.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#if defined HAVE_MALLOC_H
#   include <malloc.h>
#endif
#if defined HAVE_UNISTD_H
#   include <unistd.h>
#endif
#if defined HAVE_SYS_MMAN_H
#   include <sys/mman.h>
#endif
#if defined HAVE_LIBC_H
#   include <libc.h>
#endif

#include "libzzuf.h"
#include "lib-load.h"
#include "debug.h"
#include "fuzz.h"
#include "fd.h"

#if !defined SIGKILL
#   define SIGKILL 9
#endif

/* TODO: mremap, maybe brk/sbrk (haha) */

/* Library functions that we divert */
static void *  (*ORIG(calloc))   (size_t nmemb, size_t size);
static void *  (*ORIG(malloc))   (size_t size);
static void    (*ORIG(free))     (void *ptr);
#if defined HAVE_VALLOC
static void *  (*ORIG(valloc))   (size_t size);
#endif
#if defined HAVE_MEMALIGN
static void *  (*ORIG(memalign)) (size_t boundary, size_t size);
#endif
#if defined HAVE_POSIX_MEMALIGN
static int     (*ORIG(posix_memalign)) (void **memptr, size_t alignment,
                                        size_t size);
#endif
static void *  (*ORIG(realloc))  (void *ptr, size_t size);

#if defined HAVE_MMAP
static void *  (*ORIG(mmap))     (void *start, size_t length, int prot,
                                  int flags, int fd, off_t offset);
#endif
#if defined HAVE_MMAP64
static void *  (*ORIG(mmap64))   (void *start, size_t length, int prot,
                                  int flags, int fd, off64_t offset);
#endif
#if defined HAVE_MUNMAP
static int     (*ORIG(munmap))   (void *start, size_t length);
#endif
#if defined HAVE_MAP_FD
static kern_return_t (*ORIG(map_fd)) (int fd, vm_offset_t offset,
                                      vm_offset_t *addr, boolean_t find_space,
                                      vm_size_t numbytes);
#endif

/* We need a static memory buffer because some functions call memory
 * allocation routines before our library is loaded. Hell, even dlsym()
 * calls calloc(), so we need to do something about it */
#define DUMMY_BYTES 655360 /* 640 kB ought to be enough for anybody */
static uint64_t dummy_buffer[DUMMY_BYTES / 8];
static int64_t dummy_offset = 0;
#define DUMMY_START ((uintptr_t)dummy_buffer)
#define DUMMY_STOP ((uintptr_t)dummy_buffer + DUMMY_BYTES)

void _zz_mem_init(void)
{
    LOADSYM(calloc);
    LOADSYM(malloc);
    LOADSYM(realloc);
}

void *NEW(calloc)(size_t nmemb, size_t size)
{
    void *ret;
    if(!ORIG(calloc))
    {
        ret = dummy_buffer + dummy_offset;
        memset(ret, 0, (nmemb * size + 7) / 8);
        dummy_offset += (nmemb * size + 7) / 8;
        debug("%s(%li, %li) = %p", __func__,
              (long int)nmemb, (long int)size, ret);
        return ret;
    }
    ret = ORIG(calloc)(nmemb, size);
    if(ret == NULL && _zz_memory && errno == ENOMEM)
        raise(SIGKILL);
    return ret;
}

void *NEW(malloc)(size_t size)
{
    void *ret;
    if(!ORIG(malloc))
    {
        ret = dummy_buffer + dummy_offset;
        dummy_offset += (size + 7) / 8;
        debug("%s(%li) = %p", __func__, (long int)size, ret);
        return ret;
    }
    ret = ORIG(malloc)(size);
    if(ret == NULL && _zz_memory && errno == ENOMEM)
        raise(SIGKILL);
    return ret;
}

void NEW(free)(void *ptr)
{
    if((uintptr_t)ptr >= DUMMY_START && (uintptr_t)ptr < DUMMY_STOP)
    {
        debug("%s(%p)", __func__, ptr);
        return;
    }
    LOADSYM(free);
    ORIG(free)(ptr);
}

void *NEW(realloc)(void *ptr, size_t size)
{
    void *ret;
    if(!ORIG(realloc)
        || ((uintptr_t)ptr >= DUMMY_START && (uintptr_t)ptr < DUMMY_STOP))
    {
        ret = dummy_buffer + dummy_offset;
        memcpy(ret, ptr, size);
        dummy_offset += (size + 7) * 8;
        debug("%s(%p, %li) = %p", __func__, ptr, (long int)size, ret);
        return ret;
    }
    LOADSYM(realloc);
    ret = ORIG(realloc)(ptr, size);
    if(ret == NULL && _zz_memory && errno == ENOMEM)
        raise(SIGKILL);
    return ret;
}

#if defined HAVE_VALLOC
void *NEW(valloc)(size_t size)
{
    void *ret;
    LOADSYM(valloc);
    ret = ORIG(valloc)(size);
    if(ret == NULL && _zz_memory && errno == ENOMEM)
        raise(SIGKILL);
    return ret;
}
#endif

#if defined HAVE_MEMALIGN
void *NEW(memalign)(size_t boundary, size_t size)
{
    void *ret;
    LOADSYM(memalign);
    ret = ORIG(memalign)(boundary, size);
    if(ret == NULL && _zz_memory && errno == ENOMEM)
        raise(SIGKILL);
    return ret;
}
#endif

#if defined HAVE_POSIX_MEMALIGN
int NEW(posix_memalign)(void **memptr, size_t alignment, size_t size)
{
    int ret;
    LOADSYM(posix_memalign);
    ret = ORIG(posix_memalign)(memptr, alignment, size);
    if(ret == ENOMEM && _zz_memory)
        raise(SIGKILL);
    return ret;
}
#endif

/* Table used for mmap() and munmap() */
void **maps = NULL;
int nbmaps = 0;

#define MMAP(fn, off_t) \
    do { \
        LOADSYM(fn); \
        ret = ORIG(fn)(start, length, prot, flags, fd, offset); \
        if(!_zz_ready || !_zz_iswatched(fd) || _zz_islocked(fd)) \
            return ret; \
        if(ret && length) \
        { \
            char *b = malloc(length); \
            int i, oldpos; \
            for(i = 0; i < nbmaps; i += 2) \
                if(maps[i] == NULL) \
                    break; \
            if(i == nbmaps) \
            { \
                nbmaps += 2; \
                maps = realloc(maps, nbmaps * sizeof(void *)); \
            } \
            maps[i] = b; \
            maps[i + 1] = ret; \
            oldpos = _zz_getpos(fd); \
            _zz_setpos(fd, offset); /* mmap() maps the fd at offset 0 */ \
            memcpy(b, ret, length); /* FIXME: get rid of this */ \
            _zz_fuzz(fd, (uint8_t *)b, length); \
            _zz_setpos(fd, oldpos); \
            ret = b; \
            if(length >= 4) \
                debug("%s(%p, %li, %i, %i, %i, %lli) = %p \"%c%c%c%c...", \
                      __func__, start, (long int)length, prot, flags, fd, \
                      (long long int)offset, ret, b[0], b[1], b[2], b[3]); \
            else \
                debug("%s(%p, %li, %i, %i, %i, %lli) = %p \"%c...", \
                      __func__, start, (long int)length, prot, flags, fd, \
                      (long long int)offset, ret, b[0]); \
        } \
        else \
            debug("%s(%p, %li, %i, %i, %i, %lli) = %p", \
                  __func__, start, (long int)length, prot, flags, fd, \
                  (long long int)offset, ret); \
    } while(0)

#if defined HAVE_MMAP
void *NEW(mmap)(void *start, size_t length, int prot, int flags,
                int fd, off_t offset)
{
    void *ret; MMAP(mmap, off_t); return ret;
}
#endif

#if defined HAVE_MMAP64
void *NEW(mmap64)(void *start, size_t length, int prot, int flags,
                  int fd, off64_t offset)
{
    void *ret; MMAP(mmap64, off64_t); return ret;
}
#endif

#if defined HAVE_MUNMAP
int NEW(munmap)(void *start, size_t length)
{
    int ret, i;

    LOADSYM(munmap);
    for(i = 0; i < nbmaps; i++)
    {
        if(maps[i] != start)
            continue;

        free(start);
        ret = ORIG(munmap)(maps[i + 1], length);
        maps[i] = NULL;
        maps[i + 1] = NULL;
        debug("%s(%p, %li) = %i", __func__, start, (long int)length, ret);
        return ret;
    }

    return ORIG(munmap)(start, length);
}
#endif

#if defined HAVE_MAP_FD
kern_return_t NEW(map_fd)(int fd, vm_offset_t offset, vm_offset_t *addr,
                          boolean_t find_space, vm_size_t numbytes)
{
    kern_return_t ret;

    LOADSYM(map_fd);
    ret = ORIG(map_fd)(fd, offset, addr, find_space, numbytes);
    if(!_zz_ready || !_zz_iswatched(fd) || _zz_islocked(fd))
        return ret;

    if(ret == 0 && numbytes)
    {
        /* FIXME: do we also have to rewind the filedescriptor like in mmap? */
        char *b = malloc(numbytes);
        memcpy(b, (void *)*addr, numbytes);
        _zz_fuzz(fd, (void *)b, numbytes);
        *addr = (vm_offset_t)b;
        /* FIXME: the map is never freed; there is no such thing as unmap_fd,
         * but I suppose that kind of map should go when the filedescriptor is
         * closed (unlike mmap, which returns a persistent buffer). */

        if(numbytes >= 4)
           debug("%s(%i, %lli, &%p, %i, %lli) = %i \"%c%c%c%c", __func__,
                 fd, (long long int)offset, (void *)*addr, (int)find_space,
                 (long long int)numbytes, ret, b[0], b[1], b[2], b[3]);
        else
           debug("%s(%i, %lli, &%p, %i, %lli) = %i \"%c", __func__, fd,
                 (long long int)offset, (void *)*addr, (int)find_space,
                 (long long int)numbytes, ret, b[0]);
    }
    else
        debug("%s(%i, %lli, &%p, %i, %lli) = %i", __func__, fd,
              (long long int)offset, (void *)*addr, (int)find_space,
              (long long int)numbytes, ret);

    return ret;
}
#endif

