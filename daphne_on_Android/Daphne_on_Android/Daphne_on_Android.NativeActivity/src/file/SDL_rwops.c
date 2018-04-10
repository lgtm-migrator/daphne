/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2016 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
/* Need this so Linux systems define fseek64o, ftell64o and off64_t */
#define _LARGEFILE64_SOURCE
#include "../SDL_internal.h"

#if defined(__WIN32__)
#include "../core/windows/SDL_windows.h"
#endif


/* This file provides a general interface for SDL to read and write
   data sources.  It can easily be extended to files, memory, etc.
*/

#include "SDL_endian.h"
#include "SDL_rwops.h"

#ifdef __APPLE__
#include "cocoa/SDL_rwopsbundlesupport.h"
#endif /* __APPLE__ */

#ifdef __ANDROID__
#include "../core/android/SDL_android.h"
#include "SDL_system.h"

// RJS - 2017.08.10 - Added logging, this really should be in aSDL global .h.
#include <android/log.h>
#define LOG_TAG "SDL_android"
#define LOGI(...)  ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGE(...)  ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))
#else
#define LOGI(...)
#define LOGE(...)
#endif

#if __NACL__
#include "nacl_io/nacl_io.h"
#endif

#ifdef __WIN32__

/* Functions to read/write Win32 API file pointers */

#ifndef INVALID_SET_FILE_POINTER
#define INVALID_SET_FILE_POINTER 0xFFFFFFFF
#endif

#define READAHEAD_BUFFER_SIZE   1024

static int SDLCALL
windows_file_open(SDL_RWops * context, const char *filename, const char *mode)
{
    UINT old_error_mode;
    HANDLE h;
    DWORD r_right, w_right;
    DWORD must_exist, truncate;
    int a_mode;

    if (!context)
        return -1;              /* failed (invalid call) */

    context->hidden.windowsio.h = INVALID_HANDLE_VALUE;   /* mark this as unusable */
    context->hidden.windowsio.buffer.data = NULL;
    context->hidden.windowsio.buffer.size = 0;
    context->hidden.windowsio.buffer.left = 0;

    /* "r" = reading, file must exist */
    /* "w" = writing, truncate existing, file may not exist */
    /* "r+"= reading or writing, file must exist            */
    /* "a" = writing, append file may not exist             */
    /* "a+"= append + read, file may not exist              */
    /* "w+" = read, write, truncate. file may not exist    */

    must_exist = (SDL_strchr(mode, 'r') != NULL) ? OPEN_EXISTING : 0;
    truncate = (SDL_strchr(mode, 'w') != NULL) ? CREATE_ALWAYS : 0;
    r_right = (SDL_strchr(mode, '+') != NULL
               || must_exist) ? GENERIC_READ : 0;
    a_mode = (SDL_strchr(mode, 'a') != NULL) ? OPEN_ALWAYS : 0;
    w_right = (a_mode || SDL_strchr(mode, '+')
               || truncate) ? GENERIC_WRITE : 0;

    if (!r_right && !w_right)   /* inconsistent mode */
        return -1;              /* failed (invalid call) */

    context->hidden.windowsio.buffer.data =
        (char *) SDL_malloc(READAHEAD_BUFFER_SIZE);
    if (!context->hidden.windowsio.buffer.data) {
        return SDL_OutOfMemory();
    }
    /* Do not open a dialog box if failure */
    old_error_mode =
        SetErrorMode(SEM_NOOPENFILEERRORBOX | SEM_FAILCRITICALERRORS);

    {
        LPTSTR tstr = WIN_UTF8ToString(filename);
        h = CreateFile(tstr, (w_right | r_right),
                       (w_right) ? 0 : FILE_SHARE_READ, NULL,
                       (must_exist | truncate | a_mode),
                       FILE_ATTRIBUTE_NORMAL, NULL);
        SDL_free(tstr);
    }

    /* restore old behavior */
    SetErrorMode(old_error_mode);

    if (h == INVALID_HANDLE_VALUE) {
        SDL_free(context->hidden.windowsio.buffer.data);
        context->hidden.windowsio.buffer.data = NULL;
        SDL_SetError("Couldn't open %s", filename);
        return -2;              /* failed (CreateFile) */
    }
    context->hidden.windowsio.h = h;
    context->hidden.windowsio.append = a_mode ? SDL_TRUE : SDL_FALSE;

    return 0;                   /* ok */
}

static Sint64 SDLCALL
windows_file_size(SDL_RWops * context)
{
    LARGE_INTEGER size;

    if (!context || context->hidden.windowsio.h == INVALID_HANDLE_VALUE) {
        return SDL_SetError("windows_file_size: invalid context/file not opened");
    }

    if (!GetFileSizeEx(context->hidden.windowsio.h, &size)) {
        return WIN_SetError("windows_file_size");
    }

    return size.QuadPart;
}

static Sint64 SDLCALL
windows_file_seek(SDL_RWops * context, Sint64 offset, int whence)
{
    DWORD windowswhence;
    LARGE_INTEGER windowsoffset;

    if (!context || context->hidden.windowsio.h == INVALID_HANDLE_VALUE) {
        return SDL_SetError("windows_file_seek: invalid context/file not opened");
    }

    /* FIXME: We may be able to satisfy the seek within buffered data */
    if (whence == RW_SEEK_CUR && context->hidden.windowsio.buffer.left) {
        offset -= (long)context->hidden.windowsio.buffer.left;
    }
    context->hidden.windowsio.buffer.left = 0;

    switch (whence) {
    case RW_SEEK_SET:
        windowswhence = FILE_BEGIN;
        break;
    case RW_SEEK_CUR:
        windowswhence = FILE_CURRENT;
        break;
    case RW_SEEK_END:
        windowswhence = FILE_END;
        break;
    default:
        return SDL_SetError("windows_file_seek: Unknown value for 'whence'");
    }

    windowsoffset.QuadPart = offset;
    if (!SetFilePointerEx(context->hidden.windowsio.h, windowsoffset, &windowsoffset, windowswhence)) {
        return WIN_SetError("windows_file_seek");
    }
    return windowsoffset.QuadPart;
}

static size_t SDLCALL
windows_file_read(SDL_RWops * context, void *ptr, size_t size, size_t maxnum)
{
    size_t total_need;
    size_t total_read = 0;
    size_t read_ahead;
    DWORD byte_read;

    total_need = size * maxnum;

    if (!context || context->hidden.windowsio.h == INVALID_HANDLE_VALUE
        || !total_need)
        return 0;

    if (context->hidden.windowsio.buffer.left > 0) {
        void *data = (char *) context->hidden.windowsio.buffer.data +
            context->hidden.windowsio.buffer.size -
            context->hidden.windowsio.buffer.left;
        read_ahead =
            SDL_min(total_need, context->hidden.windowsio.buffer.left);
        SDL_memcpy(ptr, data, read_ahead);
        context->hidden.windowsio.buffer.left -= read_ahead;

        if (read_ahead == total_need) {
            return maxnum;
        }
        ptr = (char *) ptr + read_ahead;
        total_need -= read_ahead;
        total_read += read_ahead;
    }

    if (total_need < READAHEAD_BUFFER_SIZE) {
        if (!ReadFile
            (context->hidden.windowsio.h, context->hidden.windowsio.buffer.data,
             READAHEAD_BUFFER_SIZE, &byte_read, NULL)) {
            SDL_Error(SDL_EFREAD);
            return 0;
        }
        read_ahead = SDL_min(total_need, (int) byte_read);
        SDL_memcpy(ptr, context->hidden.windowsio.buffer.data, read_ahead);
        context->hidden.windowsio.buffer.size = byte_read;
        context->hidden.windowsio.buffer.left = byte_read - read_ahead;
        total_read += read_ahead;
    } else {
        if (!ReadFile
            (context->hidden.windowsio.h, ptr, (DWORD)total_need, &byte_read, NULL)) {
            SDL_Error(SDL_EFREAD);
            return 0;
        }
        total_read += byte_read;
    }
    return (total_read / size);
}

static size_t SDLCALL
windows_file_write(SDL_RWops * context, const void *ptr, size_t size,
                 size_t num)
{

    size_t total_bytes;
    DWORD byte_written;
    size_t nwritten;

    total_bytes = size * num;

    if (!context || context->hidden.windowsio.h == INVALID_HANDLE_VALUE
        || total_bytes <= 0 || !size)
        return 0;

    if (context->hidden.windowsio.buffer.left) {
        SetFilePointer(context->hidden.windowsio.h,
                       -(LONG)context->hidden.windowsio.buffer.left, NULL,
                       FILE_CURRENT);
        context->hidden.windowsio.buffer.left = 0;
    }

    /* if in append mode, we must go to the EOF before write */
    if (context->hidden.windowsio.append) {
        if (SetFilePointer(context->hidden.windowsio.h, 0L, NULL, FILE_END) ==
            INVALID_SET_FILE_POINTER) {
            SDL_Error(SDL_EFWRITE);
            return 0;
        }
    }

    if (!WriteFile
        (context->hidden.windowsio.h, ptr, (DWORD)total_bytes, &byte_written, NULL)) {
        SDL_Error(SDL_EFWRITE);
        return 0;
    }

    nwritten = byte_written / size;
    return nwritten;
}

static int SDLCALL
windows_file_close(SDL_RWops * context)
{

    if (context) {
        if (context->hidden.windowsio.h != INVALID_HANDLE_VALUE) {
            CloseHandle(context->hidden.windowsio.h);
            context->hidden.windowsio.h = INVALID_HANDLE_VALUE;   /* to be sure */
        }
        SDL_free(context->hidden.windowsio.buffer.data);
        context->hidden.windowsio.buffer.data = NULL;
        SDL_FreeRW(context);
    }
    return 0;
}
#endif /* __WIN32__ */

#ifdef HAVE_STDIO_H

/* Functions to read/write stdio file pointers */

static Sint64 SDLCALL
stdio_size(SDL_RWops * context)
{
    Sint64 pos, size;

    pos = SDL_RWseek(context, 0, RW_SEEK_CUR);
    if (pos < 0) {
        return -1;
    }
    size = SDL_RWseek(context, 0, RW_SEEK_END);

    SDL_RWseek(context, pos, RW_SEEK_SET);
    return size;
}

static Sint64 SDLCALL
stdio_seek(SDL_RWops * context, Sint64 offset, int whence)
{
#ifdef HAVE_FSEEKO64
    if (fseeko64(context->hidden.stdio.fp, (off64_t)offset, whence) == 0) {
        return ftello64(context->hidden.stdio.fp);
    }
#elif defined(HAVE_FSEEKO)
    if (fseeko(context->hidden.stdio.fp, (off_t)offset, whence) == 0) {
        return ftello(context->hidden.stdio.fp);
    }
#elif defined(HAVE__FSEEKI64)
    if (_fseeki64(context->hidden.stdio.fp, offset, whence) == 0) {
        return _ftelli64(context->hidden.stdio.fp);
    }
#else
    if (fseek(context->hidden.stdio.fp, offset, whence) == 0) {
        return ftell(context->hidden.stdio.fp);
    }
#endif
    return SDL_Error(SDL_EFSEEK);
}

static size_t SDLCALL
stdio_read(SDL_RWops * context, void *ptr, size_t size, size_t maxnum)
{
    size_t nread;

    nread = fread(ptr, size, maxnum, context->hidden.stdio.fp);
    if (nread == 0 && ferror(context->hidden.stdio.fp)) {
        SDL_Error(SDL_EFREAD);
    }
    return nread;
}

static size_t SDLCALL
stdio_write(SDL_RWops * context, const void *ptr, size_t size, size_t num)
{
    size_t nwrote;

    nwrote = fwrite(ptr, size, num, context->hidden.stdio.fp);
    if (nwrote == 0 && ferror(context->hidden.stdio.fp)) {
        SDL_Error(SDL_EFWRITE);
    }
    return nwrote;
}

static int SDLCALL
stdio_close(SDL_RWops * context)
{
    int status = 0;
    if (context) {
        if (context->hidden.stdio.autoclose) {
            /* WARNING:  Check the return value here! */
            if (fclose(context->hidden.stdio.fp) != 0) {
                status = SDL_Error(SDL_EFWRITE);
            }
        }
        SDL_FreeRW(context);
    }
    return status;
}
#endif /* !HAVE_STDIO_H */

/* Functions to read/write memory pointers */

static Sint64 SDLCALL
mem_size(SDL_RWops * context)
{
    return (Sint64)(context->hidden.mem.stop - context->hidden.mem.base);
}

static Sint64 SDLCALL
mem_seek(SDL_RWops * context, Sint64 offset, int whence)
{
    Uint8 *newpos;

    switch (whence) {
    case RW_SEEK_SET:
        newpos = context->hidden.mem.base + offset;
        break;
    case RW_SEEK_CUR:
        newpos = context->hidden.mem.here + offset;
        break;
    case RW_SEEK_END:
        newpos = context->hidden.mem.stop + offset;
        break;
    default:
        return SDL_SetError("Unknown value for 'whence'");
    }
    if (newpos < context->hidden.mem.base) {
        newpos = context->hidden.mem.base;
    }
    if (newpos > context->hidden.mem.stop) {
        newpos = context->hidden.mem.stop;
    }
    context->hidden.mem.here = newpos;
    return (Sint64)(context->hidden.mem.here - context->hidden.mem.base);
}

static size_t SDLCALL
mem_read(SDL_RWops * context, void *ptr, size_t size, size_t maxnum)
{
    size_t total_bytes;
    size_t mem_available;

    total_bytes = (maxnum * size);
    if ((maxnum <= 0) || (size <= 0)
        || ((total_bytes / maxnum) != (size_t) size)) {
        return 0;
    }

    mem_available = (context->hidden.mem.stop - context->hidden.mem.here);
    if (total_bytes > mem_available) {
        total_bytes = mem_available;
    }

    SDL_memcpy(ptr, context->hidden.mem.here, total_bytes);
    context->hidden.mem.here += total_bytes;

    return (total_bytes / size);
}

static size_t SDLCALL
mem_write(SDL_RWops * context, const void *ptr, size_t size, size_t num)
{
    if ((context->hidden.mem.here + (num * size)) > context->hidden.mem.stop) {
        num = (context->hidden.mem.stop - context->hidden.mem.here) / size;
    }
    SDL_memcpy(context->hidden.mem.here, ptr, num * size);
    context->hidden.mem.here += num * size;
    return num;
}

static size_t SDLCALL
mem_writeconst(SDL_RWops * context, const void *ptr, size_t size, size_t num)
{
    SDL_SetError("Can't write to read-only memory");
    return 0;
}

static int SDLCALL
mem_close(SDL_RWops * context)
{
    if (context) {
        SDL_FreeRW(context);
    }
    return 0;
}


// RJS - 2017.08.10 - Who formats functions like this.  Anyways, the way I'm reading this right now,
// is it first tries to open the file using STDIO and if that doesn't work then it uses the 
// Android asset system.  This won't work for a RA core, so I'm going to log and see if it's
// easy to remove.
/* Functions to create SDL_RWops structures from various data sources */
SDL_RWops *
SDL_RWFromFile(const char *file, const char *mode)
{
	LOGI("daphne-libretro: In %s, top of routine, filename: %s", __func__, file);
	
	SDL_RWops *rwops = NULL;
    if (!file || !*file || !mode || !*mode) {
        SDL_SetError("SDL_RWFromFile(): No file or no mode specified");
        return NULL;
    }

#if defined(__ANDROID__)
#ifdef HAVE_STDIO_H
    /* Try to open the file on the filesystem first */
	LOGI("daphne-libretro: In %s, deciding if should open file using relative or absolute path.", __func__);
	if (*file == '/') {
		LOGI("daphne-libretro: In %s, trying to fopen file using absolute path.", __func__);
		FILE *fp = fopen(file, mode);
        if (fp) {
			LOGI("daphne-libretro: In %s, trying to fopen file.  Success.  Going to SDL_RWFromFP.", __func__);
			return SDL_RWFromFP(fp, 1);
        }
    } else {
        /* Try opening it from internal storage if it's a relative path */
		LOGI("daphne-libretro: In %s, trying to fopen file using relative path.  Allocating memory to hold path.", __func__);
		char *path;
        FILE *fp;

        path = SDL_stack_alloc(char, PATH_MAX);
        if (path) {
			memset(path, 0, PATH_MAX);
			LOGI("daphne-libretro: In %s, allocating memory to hold path, success.", __func__);
			SDL_snprintf(path, PATH_MAX, "%s/%s",
                         SDL_AndroidGetInternalStoragePath(), file);
			LOGI("daphne-libretro: In %s, trying to fopen file.  Path: %s", __func__, path);
			fp = fopen(path, mode);
            SDL_stack_free(path);
            if (fp) {
				LOGI("daphne-libretro: In %s, trying to fopen file.  Suceess.  Going to SDL_RWFromFP.", __func__);
				return SDL_RWFromFP(fp, 1);
            }
        }
    }
#endif /* HAVE_STDIO_H */

	LOGI("daphne-libretro: In %s, no STDIO file open methods worked.", __func__);

#elif defined(__WIN32__)
    rwops = SDL_AllocRW();
    if (!rwops)
        return NULL;            /* SDL_SetError already setup by SDL_AllocRW() */
    if (windows_file_open(rwops, file, mode) < 0) {
        SDL_FreeRW(rwops);
        return NULL;
    }
    rwops->size = windows_file_size;
    rwops->seek = windows_file_seek;
    rwops->read = windows_file_read;
    rwops->write = windows_file_write;
    rwops->close = windows_file_close;
    rwops->type = SDL_RWOPS_WINFILE;

#elif HAVE_STDIO_H
    {
        #ifdef __APPLE__
        FILE *fp = SDL_OpenFPFromBundleOrFallback(file, mode);
        #elif __WINRT__
        FILE *fp = NULL;
        fopen_s(&fp, file, mode);
        #else
        FILE *fp = fopen(file, mode);
        #endif
        if (fp == NULL) {
            SDL_SetError("Couldn't open %s", file);
        } else {
            rwops = SDL_RWFromFP(fp, 1);
        }
    }
#else
    SDL_SetError("SDL not compiled with stdio support");
#endif /* !HAVE_STDIO_H */

    return rwops;
}

#ifdef HAVE_STDIO_H
SDL_RWops *
SDL_RWFromFP(FILE * fp, SDL_bool autoclose)
{
	LOGI("daphne-libretro: In %s, top of routine, file pointer: %d", __func__, (int) fp);
	SDL_RWops *rwops = NULL;

    rwops = SDL_AllocRW();
    if (rwops != NULL) {
        rwops->size = stdio_size;
        rwops->seek = stdio_seek;
        rwops->read = stdio_read;
        rwops->write = stdio_write;
        rwops->close = stdio_close;
        rwops->hidden.stdio.fp = fp;
        rwops->hidden.stdio.autoclose = autoclose;
        rwops->type = SDL_RWOPS_STDFILE;
    }
	LOGI("daphne-libretro: In %s, bottom of routine, file size: %d", __func__, (int) rwops);
	return rwops;
}
#else
SDL_RWops *
SDL_RWFromFP(void * fp, SDL_bool autoclose)
{
    SDL_SetError("SDL not compiled with stdio support");
    return NULL;
}
#endif /* HAVE_STDIO_H */

SDL_RWops *
SDL_RWFromMem(void *mem, int size)
{
    SDL_RWops *rwops = NULL;
    if (!mem) {
      SDL_InvalidParamError("mem");
      return rwops;
    }
    if (!size) {
      SDL_InvalidParamError("size");
      return rwops;
    }

    rwops = SDL_AllocRW();
    if (rwops != NULL) {
        rwops->size = mem_size;
        rwops->seek = mem_seek;
        rwops->read = mem_read;
        rwops->write = mem_write;
        rwops->close = mem_close;
        rwops->hidden.mem.base = (Uint8 *) mem;
        rwops->hidden.mem.here = rwops->hidden.mem.base;
        rwops->hidden.mem.stop = rwops->hidden.mem.base + size;
        rwops->type = SDL_RWOPS_MEMORY;
    }
    return rwops;
}

SDL_RWops *
SDL_RWFromConstMem(const void *mem, int size)
{
    SDL_RWops *rwops = NULL;
    if (!mem) {
      SDL_InvalidParamError("mem");
      return rwops;
    }
    if (!size) {
      SDL_InvalidParamError("size");
      return rwops;
    }

    rwops = SDL_AllocRW();
    if (rwops != NULL) {
        rwops->size = mem_size;
        rwops->seek = mem_seek;
        rwops->read = mem_read;
        rwops->write = mem_writeconst;
        rwops->close = mem_close;
        rwops->hidden.mem.base = (Uint8 *) mem;
        rwops->hidden.mem.here = rwops->hidden.mem.base;
        rwops->hidden.mem.stop = rwops->hidden.mem.base + size;
        rwops->type = SDL_RWOPS_MEMORY_RO;
    }
    return rwops;
}

SDL_RWops *
SDL_AllocRW(void)
{
    SDL_RWops *area;

    area = (SDL_RWops *) SDL_malloc(sizeof *area);
    if (area == NULL) {
        SDL_OutOfMemory();
    } else {
        area->type = SDL_RWOPS_UNKNOWN;
    }
    return area;
}

void
SDL_FreeRW(SDL_RWops * area)
{
    SDL_free(area);
}

/* Functions for dynamically reading and writing endian-specific values */

Uint8
SDL_ReadU8(SDL_RWops * src)
{
    Uint8 value = 0;

    SDL_RWread(src, &value, sizeof (value), 1);
    return value;
}

Uint16
SDL_ReadLE16(SDL_RWops * src)
{
    Uint16 value = 0;

    SDL_RWread(src, &value, sizeof (value), 1);
    return SDL_SwapLE16(value);
}

Uint16
SDL_ReadBE16(SDL_RWops * src)
{
    Uint16 value = 0;

    SDL_RWread(src, &value, sizeof (value), 1);
    return SDL_SwapBE16(value);
}

Uint32
SDL_ReadLE32(SDL_RWops * src)
{
    Uint32 value = 0;

    SDL_RWread(src, &value, sizeof (value), 1);
    return SDL_SwapLE32(value);
}

Uint32
SDL_ReadBE32(SDL_RWops * src)
{
    Uint32 value = 0;

    SDL_RWread(src, &value, sizeof (value), 1);
    return SDL_SwapBE32(value);
}

Uint64
SDL_ReadLE64(SDL_RWops * src)
{
    Uint64 value = 0;

    SDL_RWread(src, &value, sizeof (value), 1);
    return SDL_SwapLE64(value);
}

Uint64
SDL_ReadBE64(SDL_RWops * src)
{
    Uint64 value = 0;

    SDL_RWread(src, &value, sizeof (value), 1);
    return SDL_SwapBE64(value);
}

size_t
SDL_WriteU8(SDL_RWops * dst, Uint8 value)
{
    return SDL_RWwrite(dst, &value, sizeof (value), 1);
}

size_t
SDL_WriteLE16(SDL_RWops * dst, Uint16 value)
{
    const Uint16 swapped = SDL_SwapLE16(value);
    return SDL_RWwrite(dst, &swapped, sizeof (swapped), 1);
}

size_t
SDL_WriteBE16(SDL_RWops * dst, Uint16 value)
{
    const Uint16 swapped = SDL_SwapBE16(value);
    return SDL_RWwrite(dst, &swapped, sizeof (swapped), 1);
}

size_t
SDL_WriteLE32(SDL_RWops * dst, Uint32 value)
{
    const Uint32 swapped = SDL_SwapLE32(value);
    return SDL_RWwrite(dst, &swapped, sizeof (swapped), 1);
}

size_t
SDL_WriteBE32(SDL_RWops * dst, Uint32 value)
{
    const Uint32 swapped = SDL_SwapBE32(value);
    return SDL_RWwrite(dst, &swapped, sizeof (swapped), 1);
}

size_t
SDL_WriteLE64(SDL_RWops * dst, Uint64 value)
{
    const Uint64 swapped = SDL_SwapLE64(value);
    return SDL_RWwrite(dst, &swapped, sizeof (swapped), 1);
}

size_t
SDL_WriteBE64(SDL_RWops * dst, Uint64 value)
{
    const Uint64 swapped = SDL_SwapBE64(value);
    return SDL_RWwrite(dst, &swapped, sizeof (swapped), 1);
}

/* vi: set ts=4 sw=4 expandtab: */
