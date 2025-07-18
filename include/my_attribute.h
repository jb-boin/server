/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  Helper macros used for setting different __attributes__
  on functions in a portable fashion
*/

#ifndef _my_attribute_h
#define _my_attribute_h

#if defined(__GNUC__)
# ifndef GCC_VERSION
#  define GCC_VERSION (__GNUC__ * 1000 + __GNUC_MINOR__)
# endif
#endif

/*
  Disable __attribute__() on gcc < 2.7, g++ < 3.4, and non-gcc compilers.
  Some forms of __attribute__ are actually supported in earlier versions of
  g++, but we just disable them all because we only use them to generate
  compilation warnings.
*/
#ifndef __attribute__
# if !defined(__GNUC__) && !defined(__clang__)
#  define __attribute__(A)
# elif defined(__GNUC__)
#  ifndef GCC_VERSION
#   define GCC_VERSION (__GNUC__ * 1000 + __GNUC_MINOR__)
#  endif
#  if GCC_VERSION < 2008
#   define __attribute__(A)
#  elif defined(__cplusplus) && GCC_VERSION < 3004
#   define __attribute__(A)
#  endif
# endif
#endif

/*
  __attribute__((format(...))) is only supported in gcc >= 2.8 and g++ >= 3.4
  But that's already covered by the __attribute__ tests above, so this is
  just a convenience macro.
*/
#ifndef ATTRIBUTE_FORMAT
# define ATTRIBUTE_FORMAT(style, m, n) __attribute__((format(style, m, n)))
#endif

/*

   __attribute__((format(...))) on a function pointer is not supported
   until  gcc 3.1
*/
#ifndef ATTRIBUTE_FORMAT_FPTR
# if (GCC_VERSION >= 3001)
#  define ATTRIBUTE_FORMAT_FPTR(style, m, n) ATTRIBUTE_FORMAT(style, m, n)
# else
#  define ATTRIBUTE_FORMAT_FPTR(style, m, n)
# endif /* GNUC >= 3.1 */
#endif

/* gcc 7.5.0 does not support __attribute__((no_sanitize("undefined")) */
#ifndef ATTRIBUTE_NO_UBSAN
# if (GCC_VERSION >= 8000) || defined(__clang__)
#  define ATTRIBUTE_NO_UBSAN __attribute__((no_sanitize("undefined")))
# elif (GCC_VERSION >= 6001)
#  define ATTRIBUTE_NO_UBSAN __attribute__((no_sanitize_undefined))
# else
#  define ATTRIBUTE_NO_UBSAN
# endif
#endif

/* Define pragmas to disable warnings for stack frame checking */

#ifdef __GNUC__
#define PRAGMA_DISABLE_CHECK_STACK_FRAME                     \
_Pragma("GCC diagnostic push")                               \
_Pragma("GCC diagnostic ignored \"-Wframe-larger-than=\"")

#define PRAGMA_REENABLE_CHECK_STACK_FRAME                    \
_Pragma("GCC diagnostic pop")

/*
  The following check is for older gcc version that allocates
  a lot of stack during startup that does not need to be checked
*/

#if !defined(__clang__) && __GNUC__ < 13
#define PRAGMA_DISABLE_CHECK_STACK_FRAME_EXTRA PRAGMA_DISABLE_CHECK_STACK_FRAME
#else
#define PRAGMA_DISABLE_CHECK_STACK_FRAME_EXTRA
#endif /* !defined(__clang__) && __GNUC__ < 13 */

#else /*! __GNUC__ */
#define PRAGMA_DISABLE_CHECK_STACK_FRAME
#define PRAGMA_REENABLE_CHECK_STACK_FRAME
#define PRAGMA_DISABLE_CHECK_STACK_FRAME
#define PRAGMA_DISABLE_CHECK_STACK_FRAME_EXTRA
#endif /* __GNUC__ */

#endif /* _my_attribute_h */
