/*
 * Copyright 2017 AVSystem <avsystem@avsystem.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef AVS_COMMONS_UTILS_UTILS_H
#define AVS_COMMONS_UTILS_UTILS_H

#include <avsystem/commons/defs.h>

#include <stdarg.h>
#include <stdbool.h>

#ifdef	__cplusplus
extern "C" {
#endif

/**
 * Standard guarantees RAND_MAX to be at least 0x7fff so let's
 * use it as a base for random number generators.
 */
#define AVS_RAND_MAX 0x7fff

/**
 * Returns a pseudo-random integer from range [0, AVS_RAND_MAX]. It is
 * thread-safe.
 */
int avs_rand_r(unsigned int *seed);

/** Tests whether @p value is a power of two */
static inline bool avs_is_power_of_2(size_t value) {
    return value > 0 && !(value & (value - 1));
}

/**
 * Convert a 16-bit integer between native byte order and big-endian byte order.
 *
 * Note that it is a symmetric operation, so the same function may be used for
 * conversion in either way. If the host architecture is natively big-endian,
 * this function is a no-op.
 */
uint16_t avs_convert_be16(uint16_t value);

/**
 * Convert a 32-bit integer between native byte order and big-endian byte order.
 *
 * Note that it is a symmetric operation, so the same function may be used for
 * conversion in either way. If the host architecture is natively big-endian,
 * this function is a no-op.
 */
uint32_t avs_convert_be32(uint32_t value);

/**
 * Convert a 64-bit integer between native byte order and big-endian byte order.
 *
 * Note that it is a symmetric operation, so the same function may be used for
 * conversion in either way. If the host architecture is natively big-endian,
 * this function is a no-op.
 */
uint64_t avs_convert_be64(uint64_t value);

/**
 * Wrapper around vsnprintf(), which always return a negative in case of
 * an error (which is the only thing differentiating it from vsnprintf()).
 *
 * @returns number of bytes written on success, negative value on error.
 */
int avs_simple_vsnprintf(char *out,
                         size_t out_size,
                         const char *format,
                         va_list args) AVS_F_PRINTF(3, 0);

/**
 * Wrapper around snprintf(), which always return a negative in case of
 * an error (which is the only thing differentiating it from snprintf()).
 *
 * @returns number of bytes written on success, negative value on error.
 */
int avs_simple_snprintf(char *out, size_t out_size, const char *format, ...)
        AVS_F_PRINTF(3, 4);

/**
 * Compares two strings in a case-insensitive way.
 *
 * @returns negative, zero, or positive value if <c>s1</c> is lexicographically
 *          less, equal or greater to <c>s2</c>, respectively
 */
int avs_strcasecmp(const char *s1, const char *s2);

/**
 * Compares at most <c>n</c> characters of two strings in a case-insensitive
 * way.
 *
 * @returns negative, zero, or positive value if <c>s1</c> is lexicographically
 *          less, equal or greater to <c>s2</c>, respectively
 */
int avs_strncasecmp(const char *s1, const char *s2, size_t n);

/**
 * Portable reentrant version of standard library <c>strtok()</c>; equivalent to
 * POSIX <c>strtok_r()</c>.
 */
char *avs_strtok(char *str, char *delim, char **saveptr);

/**
 * Creates a <c>malloc()</c>-allocated copy of a given string.
 */
char *avs_strdup(const char *str);

/**
 * String that contains all the standard whitespace characters.
 */
#define AVS_SPACES " \t\v\f\r\n"

/**
 * Checks for presence of the specified <c>token</c> at the current position in
 * <c>src</c>, and advances the input (moves the pointer forward) if it matches.
 *
 * Stream position is always advanced at least to the first non-space (not
 * contained in @ref AVS_SPACES) character. If the token matches
 * (case-insensitively), it is also advanced past the token <strong>and the one
 * delimiter character immediately following it</strong>.
 *
 * @param src    Pointer to a pointer to the current position in string being
 *               parsed.
 *
 * @param token  Token to match.
 *
 * @param delims Null-terminated set of delimiter characters expected after the
 *               token. Null character (end-of-string) is also implicitly
 *               treated as a valid expected delimiter (the pointer is not
 *               advanced past it in that case).
 *
 * @return The return value convention is similar to that of <c>strcmp()</c>:
 *         0 if the token matched, negative if the actual input is
 *         lexicographically less than the expected token, or positive if the
 *         input is lexicographically greater than the expected token.
 */
int avs_match_token(const char **src, const char *token, const char *delims);

/**
 * Reads and copies a string token up to a delimiter character. Those can be
 * included by using double-quotes. Backslash is supported as an escape
 * character inside the quotes.
 *
 * As many characters as possible are copied into <c>dest</c>. The string copied
 * to <c>dest</c> is always null-terminated, unless <c>dest</c> is NULL or
 * <c>dest_size</c> is set to zero.
 *
 * The <c>src</c> pointer is always advanced past the string token <strong>and
 * the one delimiter character immediately following it</strong>, regardless of
 * whether the copy in <c>dest</c> has been truncated or not. Thus, calling this
 * function with <c>dest_size</c> set to zero will just skip the current string
 * token, without copying it anywhere.
 *
 * @param src       Pointer to a pointer to the current position in the string
 *                  being parsed.
 *
 * @param dest      Destination buffer.
 *
 * @param dest_size Size of the buffer pointed to by <c>dest</c>.
 *
 * @param delims Null-terminated set of delimiter characters expected after the
 *               token. Null character (end-of-string) is also implicitly
 *               treated as a valid expected delimiter (the pointer is not
 *               advanced past it in that case).
 */
void avs_consume_quotable_token(const char **src,
                                char *dest,
                                size_t dest_size,
                                const char *delims);

#ifdef	__cplusplus
}
#endif

#endif /* AVS_COMMONS_UTILS_UTILS_H */
