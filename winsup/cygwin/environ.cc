/* environ.cc: Cygwin-adopted functions from newlib to manipulate
   process's environment.

   Copyright 1997, 1998, 1999, 2000, 2001 Red Hat, Inc.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#ifndef MUNGEPREFIX
#define MUNGEPREFIX 0 // If not zero will cause environment variable named
		      // "prefix" to be set to its win32 absolute value by 
		      // default.
#endif

#include "winsup.h"
#include <errno.h>
#include <stdlib.h>
#include <stddef.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/cygwin.h>
#include <cygwin/version.h>
#include "sync.h"
#include "sigproc.h"
#include "pinfo.h"
#include "perprocess.h"
#include "security.h"
#include "fhandler.h"
#include "path.h"
#include "cygerrno.h"
#include "dtable.h"
#include "cygheap.h"
#include "registry.h"
#include "environ.h"

extern BOOL allow_glob;
extern bool ignore_case_with_glob;
extern BOOL allow_ntea;
extern BOOL allow_smbntsec;
extern BOOL allow_winsymlinks;
extern BOOL strip_title_path;
extern int pcheck_case;
extern int subauth_id;
BOOL reset_com = TRUE;
static BOOL envcache = TRUE;

static char **lastenviron;

#define ENVMALLOC \
  (CYGWIN_VERSION_DLL_MAKE_COMBINED (user_data->api_major, user_data->api_minor) \
	  <= CYGWIN_VERSION_DLL_MALLOC_ENV)

/* List of names which are converted from dos to unix
   on the way in and back again on the way out.

   PATH needs to be here because CreateProcess uses it and gdb uses
   CreateProcess.  HOME is here because most shells use it and would be
   confused by Windows style path names.  */
static int return_MAX_PATH (const char *) {return MAX_PATH;}
static NO_COPY win_env conv_envvars[] =
  {
    {"PATH=", 5, NULL, NULL, cygwin_win32_to_posix_path_list,
     cygwin_posix_to_win32_path_list,
     cygwin_win32_to_posix_path_list_buf_size,
     cygwin_posix_to_win32_path_list_buf_size},
    {"HOME=", 5, NULL, NULL, cygwin_conv_to_full_posix_path, cygwin_conv_to_full_win32_path,
     return_MAX_PATH, return_MAX_PATH},
    {"LD_LIBRARY_PATH=", 16, NULL, NULL, cygwin_conv_to_full_posix_path,
     cygwin_conv_to_full_win32_path, return_MAX_PATH, return_MAX_PATH},
    {"TMPDIR=", 7, NULL, NULL, cygwin_conv_to_full_posix_path, cygwin_conv_to_full_win32_path,
     return_MAX_PATH, return_MAX_PATH},
    {"TMP=", 4, NULL, NULL, cygwin_conv_to_full_posix_path, cygwin_conv_to_full_win32_path,
     return_MAX_PATH, return_MAX_PATH},
    {"TEMP=", 5, NULL, NULL, cygwin_conv_to_full_posix_path, cygwin_conv_to_full_win32_path,
     return_MAX_PATH, return_MAX_PATH},
    {NULL, 0, NULL, NULL, NULL, NULL, 0, 0}
  };

static unsigned char conv_start_chars[256] = {0};

void
win_env::add_cache (const char *in_posix, const char *in_native)
{
  TRACE_IN;
  debug_printf("called by %x", ((int *)&in_posix)[-1]);
  posix = (char *) realloc (posix, strlen (in_posix) + 1);
  strcpy (posix, in_posix);
  if (in_native)
    {
      native = (char *) realloc (native, namelen + 1 + strlen (in_native));
      (void) strcpy (native, name);
      (void) strcpy (native + namelen, in_native);
    }
  else
    {
      native = (char *) realloc (native, namelen + 1 + win32_len (in_posix));
      (void) strcpy (native, name);
      towin32 (in_posix, native + namelen);
    }
  debug_printf ("posix %s", posix);
  debug_printf ("native %s", native);
}


/* Check for a "special" environment variable name.  *env is the pointer
 * to the beginning of the environment variable name.  n is the length
 * of the name including a mandatory '='.  Returns a pointer to the
 * appropriate conversion structure.
 */
win_env * __stdcall
getwinenv (const char *env, const char *in_posix)
{
  TRACE_IN;
  if (!conv_start_chars[(unsigned char)*env])
    return NULL;

  for (int i = 0; conv_envvars[i].name != NULL; i++)
    if (strncmp (env, conv_envvars[i].name, conv_envvars[i].namelen) == 0)
      {
	win_env * const we = conv_envvars + i;
	const char *val;
	if (!cur_environ () || !(val = in_posix ?: getenv(we->name)))
	  debug_printf ("can't set native for %s since no environ yet",
			we->name);
	else if (!envcache || !we->posix || strcmp (val, we->posix) != 0)
	      we->add_cache (val);
	return we;
      }
  return NULL;
}

/* Convert windows path specs to POSIX, if appropriate.
 */
static void __stdcall
posify (char **here, const char *value)
{
  TRACE_IN;
  char *src = *here;
  win_env *conv;

  if (!(conv = getwinenv (src)))
    return;

  int len = strcspn (src, "=") + 1;

  /* Turn all the items from c:<foo>;<bar> into their
     mounted equivalents - if there is one.  */

  char *outenv = (char *) malloc (1 + len + conv->posix_len (value));
  memcpy (outenv, src, len);
  conv->toposix (value, outenv + len);
  conv->add_cache (outenv + len, *value != '/' ? value : NULL);

  debug_printf ("env var converted to %s", outenv);
  *here = outenv;
  free (src);
}

/*
 * my_findenv --
 *	Returns pointer to value associated with name, if any, else NULL.
 *	Sets offset to be the offset of the name/value combination in the
 *	environment array, for use by setenv(3) and unsetenv(3).
 *	Explicitly removes '=' in argument name.
 */

static char * __stdcall
my_findenv (const char *name, int *offset)
{
  TRACE_IN;
  register int len;
  register char **p;
  const char *c;

  c = name;
  len = 0;
  while (*c && *c != '=')
    {
      c++;
      len++;
    }

  for (p = cur_environ (); *p; ++p)
    if (!strncmp (*p, name, len))
      if (*(c = *p + len) == '=')
	{
	  *offset = p - cur_environ ();
	  return (char *) (++c);
	}
  return NULL;
}

/*
 * getenv --
 *	Returns ptr to value associated with name, if any, else NULL.
 */

extern "C" char *
getenv (const char *name)
{
  TRACE_IN;
  int offset;

  return my_findenv (name, &offset);
}

extern int __stdcall
envsize (const char * const *in_envp, int debug_print)
{
  TRACE_IN;
  const char * const *envp;
  for (envp = in_envp; *envp; envp++)
    if (debug_print)
      debug_printf ("(%d) %s", (envp - in_envp), *envp);
  return (1 + envp - in_envp) * sizeof (const char *);
}

/* Takes similar arguments to setenv except that overwrite is
   either -1, 0, or 1.  0 or 1 signify that the function should
   perform similarly to setenv.  Otherwise putenv is assumed. */
static int __stdcall
_addenv (const char *name, const char *value, int overwrite)
{
  TRACE_IN;
  int issetenv = overwrite >= 0;
  int offset;
  char *p;

#if MUNGEPREFIX
  if (!strcmp (name, "prefix"))
    value = msys_p2w (value);
#endif
  unsigned int valuelen = strlen (value);
  if ((p = my_findenv (name, &offset)))
    {				/* Already exists. */
      if (!overwrite)		/* Ok to overwrite? */
	return 0;		/* No.  Wanted to add new value.  FIXME: Right return value? */

      /* We've found the offset into environ.  If this is a setenv call and
	 there is room in the current environment entry then just overwrite it.
	 Otherwise handle this case below. */
      if (issetenv && strlen (p) >= valuelen)
	{
	  strcpy (p, value);
	  SetEnvironmentVariable (name, value);
	  return 0;
	}
    }
  else
    {				/* Create new slot. */
      int sz = envsize (cur_environ ());
      int allocsz = sz + sizeof (char *);

      offset = (sz - 1) / sizeof (char *);

      /* Allocate space for additional element plus terminating NULL. */
      if (__cygwin_environ == lastenviron)
	lastenviron = __cygwin_environ = (char **) realloc (cur_environ (),
							    allocsz);
      else if ((lastenviron = (char **) malloc (allocsz)) != NULL)
	__cygwin_environ = (char **) memcpy ((char **) lastenviron,
					     __cygwin_environ, sz);

      if (!__cygwin_environ)
	{
#ifdef DEBUGGING
	  try_to_debug ();
#endif
	  return -1;		/* Oops.  No more memory. */
	}

      __cygwin_environ[offset + 1] = NULL;	/* NULL terminate. */
      update_envptrs ();	/* Update any local copies of 'environ'. */
    }

  char *envhere;
  if (!issetenv)
    /* Not setenv. Just overwrite existing. */
    envhere = cur_environ ()[offset] = (char *) (ENVMALLOC ? strdup (name) : name);
  else
    {				/* setenv */
      /* Look for an '=' in the name and ignore anything after that if found. */
      for (p = (char *) name; *p && *p != '='; p++)
	continue;

      int namelen = p - name;	/* Length of name. */
      /* Allocate enough space for name + '=' + value + '\0' */
      envhere = cur_environ ()[offset] = (char *) malloc (namelen + valuelen + 2);
      if (!envhere)
	return -1;		/* Oops.  No more memory. */

      /* Put name '=' value into current slot. */
      strncpy (envhere, name, namelen);
      envhere[namelen] = '=';
      strcpy (envhere + namelen + 1, value);
    }

  /* Update cygwin's cache, if appropriate */
  win_env *spenv;
  if ((spenv = getwinenv (envhere)))
    spenv->add_cache (value);

  SetEnvironmentVariable (name, value);
  return 0;
}

/* putenv Sets an environment variable */
extern "C" int
putenv (const char *str)
{
  TRACE_IN;
  int res;
  if ((res = check_null_empty_str (str)))
    {
      if (res == ENOENT)
	return 0;
      set_errno (res);
      return  -1;
    }
  char *eq = strchr (str, '=');
  if (eq)
    return _addenv (str, eq + 1, -1);

  /* Remove str from the environment. */
  unsetenv (str);
  SetEnvironmentVariable(str, NULL);
  return 0;
}

/* setenv -- Set the value of the environment variable "name" to be
   "value".  If overwrite is set, replace any current value.  */
extern "C" int
setenv (const char *name, const char *value, int overwrite)
{
  TRACE_IN;
  int res;
  if ((res = check_null_empty_str (value)) == EFAULT)
    {
      set_errno (res);
      return  -1;
    }
  if ((res = check_null_empty_str (name)))
    {
      if (res == ENOENT)
	return 0;
      set_errno (res);
      return  -1;
    }
  if (*value == '=')
    value++;
  return _addenv (name, value, !!overwrite);
}

/* unsetenv(name) -- Delete environment variable "name".  */
extern "C" void
unsetenv (const char *name)
{
  TRACE_IN;
  register char **e;
  int offset;

  while (my_findenv (name, &offset))	/* if set multiple times */
    /* Move up the rest of the array */
    for (e = cur_environ () + offset; ; e++)
      if (!(*e = *(e + 1)))
	break;
}

/* Turn environment variable part of a=b string into uppercase. */
static __inline__ void
ucenv (char *p, char *eq)
{
  TRACE_IN;
  /* Amazingly, NT has a case sensitive environment name list,
     but only sometimes.
     It's normal to have NT set your "Path" to something.
     Later, you set "PATH" to something else.  This alters "Path".
     But if you try and do a naive getenv on "PATH" you'll get nothing.

     So we upper case the labels here to prevent confusion later but
     we only do it for the first process in a session group. */
  for (; p < eq; p++)
    if (islower (*p))
      *p = cyg_toupper (*p);
}

/* Parse CYGWIN options */

static NO_COPY BOOL export_settings = FALSE;

enum settings
  {
    justset,
    isfunc,
    setbit,
    set_process_state,
  };

/* When BUF is:
   null or empty: disables globbing
   "ignorecase": enables case-insensitive globbing
   anything else: enables case-sensitive globbing */
static void
glob_init (const char *buf)
{
  TRACE_IN;
  if (!buf || !*buf)
    {
      allow_glob = FALSE;
      ignore_case_with_glob = FALSE;
    }
  else if (strncasematch (buf, "ignorecase", 10))
    {
      allow_glob = TRUE;
      ignore_case_with_glob = TRUE;
    }
  else
    {
      allow_glob = TRUE;
      ignore_case_with_glob = FALSE;
    }
}

static void
check_case_init (const char *buf)
{
  TRACE_IN;
  if (!buf || !*buf)
    return;

  if (strncmp (buf, "relax", 5)== 0)
    {
      pcheck_case = PCHECK_RELAXED;
      debug_printf ("File case checking set to RELAXED");
    }
  else if (strcmp (buf, "adjust")== 0)
    {
      pcheck_case = PCHECK_ADJUST;
      debug_printf ("File case checking set to ADJUST");
    }
  else if (strcmp (buf, "strict")== 0)
    {
      pcheck_case = PCHECK_STRICT;
      debug_printf ("File case checking set to STRICT");
    }
  else
    {
      debug_printf ("Wrong case checking name: %s", buf);
    }
}

void
set_file_api_mode (codepage_type cp)
{
  TRACE_IN;
  if (cp == oem_cp)
    {
      SetFileApisToOEM ();
      debug_printf ("File APIs set to OEM");
    }
  else if (cp == ansi_cp)
    {
      SetFileApisToANSI ();
      debug_printf ("File APIs set to ANSI");
    }
}

static void
codepage_init (const char *buf)
{
  TRACE_IN;
  if (!buf || !*buf)
    return;

  if (strcmp (buf, "oem")== 0)
    {
      current_codepage = oem_cp;
      set_file_api_mode (current_codepage);
    }
  else if (strcmp (buf, "ansi")== 0)
    {
      current_codepage = ansi_cp;
      set_file_api_mode (current_codepage);
    }
  else
    {
      debug_printf ("Wrong codepage name: %s", buf);
    }
}

static void
subauth_id_init (const char *buf)
{
  TRACE_IN;
  if (!buf || !*buf)
    return;

  int i = strtol (buf, NULL, 0);

  /* 0..127 are reserved by Microsoft, 132 is IIS subauthentication. */
  if (i > 127 && i != 132 && i <= 255)
    subauth_id = i;
}

static void
set_chunksize (const char *buf)
{
  wincap.set_chunksize (strtol (buf, NULL, 0));
}

/* The structure below is used to set up an array which is used to
   parse the CYGWIN environment variable or, if enabled, options from
   the registry.  */
static struct parse_thing
  {
    const char *name;
    union parse_setting
      {
	BOOL *b;
	DWORD *x;
	int *i;
	void (*func)(const char *);
      } setting;

    enum settings disposition;
    char *remember;
    union parse_values
      {
	DWORD i;
	const char *s;
      } values[2];
  } known[] NO_COPY =
{
  {"binmode", {x: &binmode}, justset, NULL, {{O_BINARY}, {O_BINARY}}},
  {"check_case", {func: &check_case_init}, isfunc, NULL, {{PCHECK_ADJUST}, {PCHECK_ADJUST}}},
  {"codepage", {func: &codepage_init}, isfunc, NULL, {{0}, {0}}},
  {"envcache", {&envcache}, justset, NULL, {{TRUE}, {FALSE}}},
  {"error_start", {func: &error_start_init}, isfunc, NULL, {{0}, {0}}},
  {"export", {&export_settings}, justset, NULL, {{FALSE}, {TRUE}}},
  {"forkchunk", {func: set_chunksize}, isfunc, NULL, {{0}, {0}}},
  {"glob", {func: &glob_init}, isfunc, NULL, {{0}, {s: "normal"}}},
  {"ntea", {&allow_ntea}, justset, NULL, {{FALSE}, {FALSE}}},
  {"ntsec", {&allow_ntsec}, justset, NULL, {{FALSE}, {FALSE}}},
  {"smbntsec", {&allow_smbntsec}, justset, NULL, {{FALSE}, {FALSE}}},
  {"reset_com", {&reset_com}, justset, NULL, {{FALSE}, {TRUE}}},
  {"strip_title", {&strip_title_path}, justset, NULL, {{FALSE}, {TRUE}}},
  {"subauth_id", {func: &subauth_id_init}, isfunc, NULL, {{0}, {0}}},
  {"title", {&display_title}, justset, NULL, {{FALSE}, {TRUE}}},
  {"tty", {NULL}, set_process_state, NULL, {{0}, {PID_USETTY}}},
  {"winsymlinks", {&allow_winsymlinks}, justset, NULL, {{FALSE}, {TRUE}}},
  {NULL, {0}, justset, 0, {{0}, {0}}}
};

/* Parse a string of the form "something=stuff somethingelse=more-stuff",
   silently ignoring unknown "somethings".  */
static void __stdcall
parse_options (char *buf)
{
  TRACE_IN;
  int istrue;
  char *p, *lasts;
  parse_thing *k;

  if (buf == NULL)
    {
      char newbuf[MAX_PATH + 7] = "CYGWIN";
      for (k = known; k->name != NULL; k++)
	if (k->remember)
	  {
	    strcat (strcat (newbuf, " "), k->remember);
	    free (k->remember);
	    k->remember = NULL;
	  }
      if (!export_settings)
	return;
      newbuf[sizeof ("CYGWIN") - 1] = '=';
      debug_printf ("%s", newbuf);
      putenv (newbuf);
      return;
    }

  buf = strcpy ((char *) alloca (strlen (buf) + 1), buf);
  for (p = strtok_r (buf, " \t", &lasts);
       p != NULL;
       p = strtok_r (NULL, " \t", &lasts))
    {
      if (!(istrue = !strncasematch (p, "no", 2)))
	p += 2;
      else if (!(istrue = *p != '-'))
	p++;

      char ch, *eq;
      if ((eq = strchr (p, '=')) != NULL || (eq = strchr (p, ':')) != NULL)
	ch = *eq, *eq++ = '\0';
      else
	ch = 0;

      for (parse_thing *k = known; k->name != NULL; k++)
	if (strcasematch (p, k->name))
	  {
	    switch (k->disposition)
	      {
	      case isfunc:
		k->setting.func ((!eq || !istrue) ?
		  k->values[istrue].s : eq);
		debug_printf ("%s (called func)", k->name);
		break;
	      case justset:
		if (!istrue || !eq)
		  *k->setting.x = k->values[istrue].i;
		else
		  *k->setting.x = strtol (eq, NULL, 0);
		debug_printf ("%s %d", k->name, *k->setting.x);
		break;
	      case set_process_state:
		k->setting.x = &myself->process_state;
		/* fall through */
	      case setbit:
		*k->setting.x &= ~k->values[istrue].i;
		if (istrue || (eq && strtol (eq, NULL, 0)))
		  *k->setting.x |= k->values[istrue].i;
		debug_printf ("%s %x", k->name, *k->setting.x);
		break;
	      }

	    if (eq)
	      *--eq = ch;

	    int n = eq - p;
	    p = strdup (p);
	    if (n > 0)
	      p[n] = ':';
	    k->remember = p;
	    break;
	  }
      }
  debug_printf ("returning");
  return;
}

/* Set options from the registry. */
static void __stdcall
regopt (const char *name)
{
  TRACE_IN;
  MALLOC_CHECK;
  /* FIXME: should not be under mount */
  reg_key r (KEY_READ, CYGWIN_INFO_PROGRAM_OPTIONS_NAME, NULL);
  char buf[MAX_PATH];
  char lname[strlen(name) + 1];
  strlwr (strcpy (lname, name));
  MALLOC_CHECK;
  if (r.get_string (lname, buf, sizeof (buf) - 1, "") == ERROR_SUCCESS)
    parse_options (buf);
  else
    {
      reg_key r1 (HKEY_LOCAL_MACHINE, KEY_READ, "SOFTWARE",
		  CYGWIN_INFO_CYGNUS_REGISTRY_NAME,
		  CYGWIN_INFO_CYGWIN_REGISTRY_NAME,
		  CYGWIN_INFO_PROGRAM_OPTIONS_NAME, NULL);
      if (r1.get_string (lname, buf, sizeof (buf) - 1, "") == ERROR_SUCCESS)
	parse_options (buf);
    }
  MALLOC_CHECK;
}

/* Initialize the environ array.  Look for the CYGWIN environment
   environment variable and set appropriate options from it.  */
void
environ_init (char **envp, int envc)
{
  TRACE_IN;
  char *rawenv;
  int i;
  char *p;
  char *newp;
  int sawTERM = 0;
  bool envp_passed_in;
  static char NO_COPY cygterm[] = "TERM=cygwin";

  static int initted;
  if (!initted)
    {
      for (int i = 0; conv_envvars[i].name != NULL; i++)
	{
	  conv_start_chars[cyg_tolower(conv_envvars[i].name[0])] = 1;
	  conv_start_chars[cyg_toupper(conv_envvars[i].name[0])] = 1;
	}
      initted = 1;
    }

#if ! __MSYS__
  regopt ("default");
  if (myself->progname[0])
    regopt (myself->progname);
#endif

#if ! __MSYS__
#ifdef NTSEC_ON_BY_DEFAULT
  /* Set ntsec explicit as default, if NT is running */
  if (wincap.has_security ())
    allow_ntsec = TRUE;
#endif
#endif /* ! __MSYS__ */

  if (!envp)
    envp_passed_in = 0;
  else
    {
      char **newenv = (char **) malloc (envc);
      memcpy (newenv, envp, envc);
      cfree (envp);

      /* Older applications relied on the fact that cygwin malloced elements of the
	 environment list.  */
      envp = newenv;
      if (ENVMALLOC)
	for (char **e = newenv; *e; e++)
	  {
	    char *p = *e;
	    *e = strdup (p);
	    cfree (p);
	  }
      envp_passed_in = 1;
      goto out;
    }

  /* Allocate space for environment + trailing NULL + CYGWIN env. */
  lastenviron = envp = (char **) malloc ((4 + (envc = 100)) * sizeof (char *));
  rawenv = GetEnvironmentStrings ();

  /* Current directory information is recorded as variables of the
     form "=X:=X:\foo\bar; these must be changed into something legal
     (we could just ignore them but maybe an application will
     eventually want to use them).  */
  for (i = 0, p = rawenv; *p != '\0'; p = strchr (p, '\0') + 1, i++)
    {
      newp = strdup (p);
      if (i >= envc)
	envp = (char **) realloc (envp, (4 + (envc += 100)) * sizeof (char *));
      envp[i] = newp;
      if (*newp == '=')
	*newp = '!';
      char *eq;
      if ((eq = strchr (newp, '=')) == NULL)
	eq = strchr (newp, '\0');
      if (!myself->ppid_handle)
	ucenv (newp, eq);
      if (*newp == 'T' && strncmp (newp, "TERM=", 5) == 0)
	sawTERM = 1;
      if (*newp == 'C' && strncmp (newp, "CYGWIN=", sizeof("CYGWIN=") - 1) == 0)
	parse_options (newp + sizeof("CYGWIN=") - 1);
#if MUNGEPREFIX
      if (*newp == 'p' && strncmp (newp, "prefix=", 6) == 0)
	_addenv ("prefix", newp + 6, 1);
#endif
      if (*eq && conv_start_chars[(unsigned char)envp[i][0]])
	posify (envp + i, *++eq ? eq : --eq);
      debug_printf ("%p: %s", envp[i], envp[i]);
    }

  if (!sawTERM)
    envp[i++] = cygterm;
  envp[i] = NULL;
  FreeEnvironmentStrings (rawenv);

out:
  __cygwin_environ = envp;
  update_envptrs ();
  if (envp_passed_in)
    {
      p = getenv ("CYGWIN");
      if (p)
	parse_options (p);
    }
  parse_options (NULL);
  MALLOC_CHECK;
}

/* Function called by qsort to sort environment strings.  */
static int
env_sort (const void *a, const void *b)
{
  TRACE_IN;
  const char **p = (const char **) a;
  const char **q = (const char **) b;

  return strcmp (*p, *q);
}

/* Keep this list in upper case and sorted */
static const NO_COPY char* forced_winenv_vars [] =
  {
    "HOMEDRIVE",
    "HOMEPATH",
    "PATH",
#if __MSYS__
    "MSYSTEM",
#endif /* __MSYS__ */
    "SYSTEMDRIVE",
    "SYSTEMROOT",
    "USERDOMAIN",
    "USERNAME",
    "USERPROFILE",
    "WINDIR",
    NULL
  };

#define FORCED_WINENV_SIZE (sizeof (forced_winenv_vars) / sizeof (forced_winenv_vars[0]))

/* Create a Windows-style environment block, i.e. a typical character buffer
   filled with null terminated strings, terminated by double null characters.
   Converts environment variables noted in conv_envvars into win32 form
   prior to placing them in the string.  */
char * __stdcall
winenv (const char * const *envp, int keep_posix)
{
  TRACE_IN;
  FIXME;
  // For some reason keep_posix appears to be reversed in the logic flow.
  // I'm adding this reversing algorithm because I've just spent days of time
  // chasing a bug that kept leading me here.
  keep_posix = keep_posix ? 0 : 1;
  // The meaning of keep_posix is supposed to allow a msys dll dependent binary
  // from needing to convert the environment to win32 format.  A ``FIXME'' in
  // the spawn.cc (spawn_guts) function asks why we need to call this function
  // for an msys dll dependent process anyway.  This function needs fixed.

  int srcplen, envc, envblocklen;
  const char * const *srcp;
  const char **dstp;
  bool saw_forced_winenv[FORCED_WINENV_SIZE] = {0};
  char *tptr;

  debug_printf ("envp %p, keep_posix %d", envp, keep_posix);

  envblocklen = 0;
  for (envc = 0; envp[envc]; envc++)
    continue;

  debug_printf ("envc = %d", envc);

  const char *newenvp[envc + 1 + FORCED_WINENV_SIZE];

  for (srcp = envp, dstp = newenvp; *srcp; srcp++, dstp++)
    {
      srcplen = strcspn (*srcp, "=");
      win_env *conv;

      if (keep_posix || !(conv = getwinenv (*srcp, *srcp + srcplen + 1)))
	*dstp = *srcp;
      else
	{
	  tptr = (char *) alloca (strlen (conv->native) + 1);
	  strcpy (tptr, conv->native);
	  *dstp = tptr;
	}
      envblocklen += strlen (*dstp) + 1;
      if ((*dstp)[0] == '!' && isdrive ((*dstp) + 1) && (*dstp)[3] == '=')
	{
	  tptr = (char *) alloca (strlen (*dstp) + 1);
	  strcpy (tptr, *dstp);
	  *tptr = '=';
	  *dstp = tptr;
	}

      for (int i = 0; forced_winenv_vars[i]; i++)
	if (!saw_forced_winenv[i])
	  saw_forced_winenv[i] = strncasematch (forced_winenv_vars[i], *srcp, srcplen);
    }

  for (int i = 0; forced_winenv_vars[i]; i++)
    if (!saw_forced_winenv[i])
      {
	srcplen = strlen (forced_winenv_vars[i]);
	tptr = (char *) alloca (srcplen + MAX_PATH + 1);
	strcpy (tptr, forced_winenv_vars[i]);
	strcat (tptr, "=");
	if (!GetEnvironmentVariable (forced_winenv_vars[i], tptr + srcplen + 1, MAX_PATH))
	  debug_printf ("warning: %s not present in environment", *srcp);
	else
	  {
	    *dstp++ = tptr;
	    envblocklen += strlen (tptr) + 1;
	  }
      }

  *dstp = NULL;		/* Terminate */

  int envlen = dstp - newenvp;
  debug_printf ("env count %d, bytes %d", envlen, envblocklen);

  /* Windows programs expect the environment block to be sorted.  */
  qsort (newenvp, envlen, sizeof (char *), env_sort);

  /* Create an environment block suitable for passing to CreateProcess.  */
  char *ptr, *envblock;
  envblock = (char *) malloc (envblocklen + 2 + (MAX_PATH * 256));
  for (srcp = newenvp, ptr = envblock; *srcp; srcp++)
    {
     if (keep_posix) {
      char * w32path = msys_p2w(*srcp);
      srcplen = strlen (w32path);
      memcpy (ptr, w32path, srcplen + 1);
      if (w32path != *srcp)
        free (w32path);
      ptr += srcplen + 1;
     } else {
      srcplen = strlen(*srcp);
      memcpy (ptr, *srcp, srcplen + 1); 
      ptr += srcplen + 1;
     }
    }
  *ptr = '\0';		/* Two null bytes at the end */

  return envblock;
}

/* This idiocy is necessary because the early implementers of cygwin
   did not seem to know about importing data variables from the DLL.
   So, we have to synchronize cygwin's idea of the environment with the
   main program's with each reference to the environment. */
extern "C" char ** __stdcall
cur_environ ()
{
  TRACE_IN;
  if (*main_environ != __cygwin_environ)
    {
      __cygwin_environ = *main_environ;
      update_envptrs ();
    }

  return __cygwin_environ;
}
