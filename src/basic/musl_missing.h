#pragma once
#ifndef ELOGIND_BASIC_MUSL_MISSING_H_INCLUDED
#define ELOGIND_BASIC_MUSL_MISSING_H_INCLUDED


/****************************************************************
 * musl_missing.h - work around glibc extensions for musl libc.
 *
 * Implements glibc functions missing in musl libc as macros.
 * Is to be included where these functions are used.
 * Also defines some glibc only constants as either 0 or
 * as found in the corresponding glibc header file.
 *
 * Juergen Buchmueller <pullmoll@t-online.de> for Void Linux
 * Public Domain; no warranties whatsoever. Thank you Mr. P.
 *
 ****************************************************************/


void elogind_set_program_name(const char* pcall);

#if !defined(__GLIBC__)
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define strerror_r(e, m, k) (strerror_r(e, m, k) < 0 ? strdup("strerror_r() failed") : m);

#ifndef _ERRNO_H
extern char *program_invocation_name;
extern char *program_invocation_short_name;
#endif // errno.h included beforehand

/*
 * Possibly TODO according to http://man7.org/linux/man-pages/man3/getenv.3.html
 * + test if the process's effective user ID does not match its real user ID or
 *   the process's effective group ID does not match its real group ID;
 *   typically this is the result of executing a set-user-ID or set-
 *   group-ID program. Is calling issetugid() sufficient here?
 * + test if the effective capability bit was set on the executable file
 * + test if the process has a nonempty permitted capability set
 */
#if !defined(HAVE_SECURE_GETENV) && !defined(HAVE___SECURE_GETENV)
#  define secure_getenv(name) \
        (issetugid() ? NULL : getenv(name))
#  define HAVE_SECURE_GETENV 1
#endif // HAVE_[__]SECURE_GETENV

/* Poor man's basename */
#define basename(path) \
        (strrchr(path, '/') ? strrchr(path, '/')+1 : path)

/* strndupa may already be defined in another compatibility header */
#if !defined(strndupa)
#define strndupa(src, n) \
        (__extension__ ({const char *in = (src);   \
                size_t len = strnlen(in, (n)) + 1; \
                char *out = (char *) alloca(len);  \
                out[len-1] = '\0';                 \
                (char *) memcpy(out, in, len-1);}) \
        )
#endif

/* See http://man7.org/linux/man-pages/man3/canonicalize_file_name.3.html */
#define canonicalize_file_name(path) \
        realpath(path, NULL)

/* GLOB_BRACE is another glibc extension - ignore it for musl libc */
#define GLOB_BRACE 0

/* getnameinfo(3) glibc extensions are undefined in musl libc */
#define NI_IDN 0
#define NI_IDN_USE_STD3_ASCII_RULES 0

/* Taken from glibc's net/if_arp.h */
#if !defined(ARPHRD_IEEE802154_PHY)
#define ARPHRD_IEEE802154_PHY 805        /* IEEE 802.15.4 PHY header.  */
#endif

/* Shorthand for type of comparison functions. */
#ifndef __COMPAR_FN_T
# define __COMPAR_FN_T
typedef int (*__compar_fn_t) (const void *, const void *);
typedef __compar_fn_t comparison_fn_t;
#endif


#endif // !defined(__GLIBC__)

#endif // ELOGIND_BASIC_MUSL_MISSING_H_INCLUDED

