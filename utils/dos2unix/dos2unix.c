 *  Version: $$Id: dos2unix.c,v 1.3 2002-10-06 09:55:42 jrfonseca Exp $$
#define RCS_DATE     "$$Date: 2002-10-06 09:55:42 $$"
#define RCS_REVISION "$$Revision: 1.3 $$"
#include "dos2unix.h"

#ifdef HAVE_CONFIG_H
#  if HAVE_DIRENT_H
#	  include <dirent.h>
#	  define NAMLEN(dirent) strlen((dirent)->d_name)
#  else
#	  define dirent direct
#	  define NAMLEN(dirent) (dirent)->d_namlen
#	  if HAVE_SYS_NDIR_H
#	   include <sys/ndir.h>
#	  endif
#	  if HAVE_SYS_DIR_H
#	   include <sys/dir.h>
#	  endif
#	  if HAVE_NDIR_H
#	   include <ndir.h>
#	  endif
#  endif
#else
#  if defined(__MSDOS__) || defined(_WIN32)
#    include <dir.h>
#  endif
#endif
#ifdef HAVE_CONFIG_H
#  if HAVE_UTIME_H
#   include <utime.h>
#  endif
#  if HAVE_SYS_UTIME_H
#   include <sys/utime.h>
#  endif
#else
#  include <utime.h>
#endif /* HAVE_CONFIG_H */


#if defined(__MSDOS__) || defined(_WIN32)
#  define R_CNTRL   "rb"
#  define W_CNTRL   "wb"
#else
#  define R_CNTRL   "r"
#  define W_CNTRL   "w"
#endif
#endif /* DEBUG */
#endif /* DEBUG */
#ifdef HAVE_MKSTEMP
  mkstemp (TempPath);
#else
  mktemp  (TempPath);
#endif
#endif /* DEBUG */
#ifdef HAVE_MKSTEMP
  mkstemp (TempPath);
#else
  mktemp  (TempPath);
#endif
#endif /* DEBUG */