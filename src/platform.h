/* platform.h — cross-platform abstraction for Glyph.
 *
 * This header abstracts the differences between Unix (Linux, macOS, BSD)
 * and Windows (MinGW). It is the ONLY place that has #ifdef _WIN32.
 * All other source files include this instead of platform-specific headers.
 *
 * The Unix way: one header, one job — hide the platform.
 */

#ifndef GLYPH_PLATFORM_H
#define GLYPH_PLATFORM_H

/* ------------------------------------------------------------------ */
/* Common headers (work everywhere)                                    */
/* ------------------------------------------------------------------ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <limits.h>
#include <errno.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Platform detection                                                 */
/* ------------------------------------------------------------------ */
#if defined(_WIN32) || defined(_MSC_VER)
  #define GLYPH_WINDOWS 1
#else
  #define GLYPH_UNIX 1
#endif

/* ------------------------------------------------------------------ */
/* Windows-specific                                                    */
/* ------------------------------------------------------------------ */
#ifdef GLYPH_WINDOWS
  #include <windows.h>
  #include <io.h>
  #include <process.h>
  
  /* Windows doesn't have sysexits.h — define the codes we use */
  #ifndef EX_OK
    #define EX_OK          0
    #define EX_USAGE       64
    #define EX_DATAERR     65
    #define EX_NOINPUT     66
    #define EX_SOFTWARE    70
  #endif
  
  /* Windows doesn't have unistd.h — minimal replacements */
  #define access _access
  #define R_OK   4
  
  /* No fork() on Windows — we use _spawnvp for system() */
  /* No signals in the Unix sense — we stub them out */
  #define SIGINT  2
  #define SIGTERM 15
  #define SIGHUP  1
  #define SIGUSR1 10
  #define SIGUSR2 12
  #define SIGPIPE 13
  
  /* signal handler type */
  typedef void (*glyph_sig_handler)(int);
  
  /* Stub sigaction — Windows uses signal() instead */
  struct glyph_sigaction {
    glyph_sig_handler sa_handler;
    int sa_flags;
    int sa_mask;
  };
  #define sigaction(sig, act, old) signal((sig), (act)->sa_handler)
  
  /* Windows doesn't have sigaction's struct, so we use a simpler approach */
  #define GLYPH_NO_SIGNALS 1
  
  /* Dynamic loading on Windows uses LoadLibrary/GetProcAddress */
  #define dlopen(name, flags) (void*)LoadLibraryA(name)
  #define dlsym(handle, name) (void*)GetProcAddress((HMODULE)(handle), name)
  #define dlclose(handle) FreeLibrary((HMODULE)(handle))
  #define dlerror() "Windows DLL error"
  #define RTLD_NOW    0
  #define RTLD_GLOBAL 0
  #define RTLD_DEFAULT NULL
  
  /* No readlink on Windows — return -1 */
  static inline ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    (void)path; (void)buf; (void)bufsiz;
    return -1;
  }
  
  /* snprintf is available on modern Windows, but be safe */
  #if defined(_MSC_VER) && _MSC_VER < 1900
    #define snprintf _snprintf
  #endif
  
  /* strsep not available on Windows */
  #ifndef strsep
  static inline char *glyph_strsep(char **stringp, const char *delim) {
    char *begin, *end;
    begin = *stringp;
    if (begin == NULL) return NULL;
    end = begin + strcspn(begin, delim);
    if (*end) { *end++ = '\0'; *stringp = end; }
    else { *stringp = NULL; }
    return begin;
  }
  #define strsep glyph_strsep
  #endif
  
  /* strtok is available but strtok_r is not */
  #define strtok_r(s, delim, save) strtok(s, delim)
  
  /* popen / pclose */
  #define popen _popen
  #define pclose _pclose
  
  /* /proc/self/exe doesn't exist on Windows — use GetModuleFileName */
  static inline ssize_t glyph_read_self_path(char *buf, size_t bufsiz) {
    DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)bufsiz);
    if (len == 0) return -1;
    return (ssize_t)len;
  }
  #define GLYPH_READ_SELF_PATH(buf, siz) glyph_read_self_path((buf), (siz))

/* ------------------------------------------------------------------ */
/* Unix-specific                                                       */
/* ------------------------------------------------------------------ */
#else
  #include <unistd.h>
  #include <sys/wait.h>
  #include <signal.h>
  #include <dlfcn.h>
  #include <sysexits.h>
  #include <sys/types.h>
  
  /* readlink for /proc/self/exe */
  #define GLYPH_READ_SELF_PATH(buf, siz) readlink("/proc/self/exe", (buf), (siz))
  
#endif /* GLYPH_UNIX */

/* ------------------------------------------------------------------ */
/* Shared utilities                                                    */
/* ------------------------------------------------------------------ */

/* portable way to check if a file exists */
static inline int glyph_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* portable path separator */
#ifdef GLYPH_WINDOWS
  #define GLYPH_PATH_SEP '\\'
  #define GLYPH_PATH_SEP_STR "\\"
#else
  #define GLYPH_PATH_SEP '/'
  #define GLYPH_PATH_SEP_STR "/"
#endif

#endif /* GLYPH_PLATFORM_H */
