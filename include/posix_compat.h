#ifndef RICTUS_INVESTIGATION_POSIX_COMPAT_H
#define RICTUS_INVESTIGATION_POSIX_COMPAT_H

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#define _stricmp strcasecmp
#define _strnicmp strncasecmp

static inline int rictus_fopen_s(FILE **file, const char *path, const char *mode)
{
    if (file == NULL) return EINVAL;
    *file = fopen(path, mode);
    return *file != NULL ? 0 : errno;
}

static inline int rictus_strcpy_s(char *destination, size_t size, const char *source)
{
    size_t length;
    if (destination == NULL || source == NULL || size == 0) return EINVAL;
    length = strlen(source);
    if (length >= size) {
        destination[0] = '\0';
        return ERANGE;
    }
    memcpy(destination, source, length + 1);
    return 0;
}

static inline int rictus_sprintf_s(char *destination, size_t size, const char *format, ...)
{
    int result;
    va_list arguments;
    if (destination == NULL || size == 0 || format == NULL) return -1;
    va_start(arguments, format);
    result = vsnprintf(destination, size, format, arguments);
    va_end(arguments);
    if (result < 0 || (size_t)result >= size) {
        destination[0] = '\0';
        return -1;
    }
    return result;
}

#define fopen_s rictus_fopen_s
#define strcpy_s rictus_strcpy_s
#define sprintf_s rictus_sprintf_s
#define strtok_s strtok_r

#endif
