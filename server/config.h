/* config.h.  Generated from config.h.in by configure.  */
/* config.h.in.  Generated from configure.ac by autoheader.  */

/* Path to the SSSD data provider plugins */
#define DATA_PROVIDER_PLUGINS_PATH ""LIBDIR"/sssd"

/* Path to the SSSD databases */
#define DB_PATH ""VARDIR"/lib/sss/db"

/* Define to 1 if translation of program messages to the user's native
   language is requested. */
#define ENABLE_NLS 1

/* Does c-ares have ares_free_data()? */
/* #undef HAVE_ARES_DATA */

/* Define to 1 if you have the <ares.h> header file. */
#define HAVE_ARES_H 1

/* Define if dbus_watch_get_unix_fd exists */
#define HAVE_DBUS_WATCH_GET_UNIX_FD 1

/* Define if the GNU dcgettext() function is already present or preinstalled.
   */
#define HAVE_DCGETTEXT 1

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* Define to 1 if the system has the type `errno_t'. */
/* #undef HAVE_ERRNO_T */

/* Define to 1 if you have the `getpgrp' function. */
#define HAVE_GETPGRP 1

/* Define if the GNU gettext() function is already present or preinstalled. */
#define HAVE_GETTEXT 1

/* Define if you have the iconv() function. */
/* #undef HAVE_ICONV */

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the `krb5_free_unparsed_name' function. */
#define HAVE_KRB5_FREE_UNPARSED_NAME 1

/* Define to 1 if you have the `krb5_get_error_message' function. */
#define HAVE_KRB5_GET_ERROR_MESSAGE 1

/* Define to 1 if you have the `krb5_get_init_creds_opt_alloc' function. */
#define HAVE_KRB5_GET_INIT_CREDS_OPT_ALLOC 1

/* Define to 1 if you have the <krb5.h> header file. */
#define HAVE_KRB5_H 1

/* Define to 1 if you have the <krb5/krb5.h> header file. */
#define HAVE_KRB5_KRB5_H 1

/* Define to 1 if you have the `ldap_control_create' function. */
#define HAVE_LDAP_CONTROL_CREATE 1

/* Define to 1 if you have the <ldb.h> header file. */
#define HAVE_LDB_H 1

/* Define to 1 if you have the <ldb_module.h> header file. */
#define HAVE_LDB_MODULE_H 1

/* Define if libpcre version is less than 7 */
/* #undef HAVE_LIBPCRE_LESSER_THAN_7 */

/* Define to 1 if the system has the type `long long'. */
#define HAVE_LONG_LONG 1

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Define to 1 if you have the <pcre.h> header file. */
#define HAVE_PCRE_H 1

/* Define to 1 if you have the <popt.h> header file. */
#define HAVE_POPT_H 1

/* Define to 1 if you have the `prctl' function. */
#define HAVE_PRCTL 1

/* Define to 1 if you have the <sasl/sasl.h> header file. */
#define HAVE_SASL_SASL_H 1

/* Define to 1 if you have the <security/pam_appl.h> header file. */
#define HAVE_SECURITY_PAM_APPL_H 1

/* Define to 1 if you have the <security/pam_misc.h> header file. */
#define HAVE_SECURITY_PAM_MISC_H 1

/* Define to 1 if you have the <security/pam_modules.h> header file. */
#define HAVE_SECURITY_PAM_MODULES_H 1

/* Build with SELinux support */
#define HAVE_SELINUX 1

/* Define to 1 if you have the <selinux/selinux.h> header file. */
#define HAVE_SELINUX_SELINUX_H 1

/* Define to 1 if you have the `sigaction' function. */
#define HAVE_SIGACTION 1

/* Define to 1 if you have the `sigblock' function. */
#define HAVE_SIGBLOCK 1

/* Define to 1 if you have the `sigprocmask' function. */
#define HAVE_SIGPROCMASK 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the <sys/inotify.h> header file. */
#define HAVE_SYS_INOTIFY_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <tdb.h> header file. */
#define HAVE_TDB_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Where to store log files for the SSSD */
#define LOG_PATH ""VARDIR"/log/sssd"

/* Define to the sub-directory in which libtool stores uninstalled libraries.
   */
#define LT_OBJDIR ".libs/"

/* Define to 1 if your C compiler doesn't accept -c and -o together. */
/* #undef NO_MINUS_C_MINUS_O */

/* Name of package */
#define PACKAGE "sss_daemon"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "sssd-devel@lists.fedorahosted.org"

/* Define to the full name of this package. */
#define PACKAGE_NAME "sss_daemon"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "sss_daemon 1.0.5"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "sss_daemon"

/* Define to the version of this package. */
#define PACKAGE_VERSION "1.0.5"

/* Where to store pid files for the SSSD */
#define PID_PATH ""VARDIR"/run"

/* Where to store pipe files for the SSSD interconnects */
#define PIPE_PATH ""VARDIR"/lib/sss/pipes"

/* Where to store pubconf files for the SSSD */
#define PUBCONF_PATH ""VARDIR"/lib/sss/pubconf"

/* The size of `char', as computed by sizeof. */
#define SIZEOF_CHAR 1

/* The size of `int', as computed by sizeof. */
#define SIZEOF_INT 4

/* The size of `long', as computed by sizeof. */
#define SIZEOF_LONG 8

/* The size of `long long', as computed by sizeof. */
#define SIZEOF_LONG_LONG 8

/* The size of `off_t', as computed by sizeof. */
#define SIZEOF_OFF_T 8

/* The size of `short', as computed by sizeof. */
#define SIZEOF_SHORT 2

/* The size of `size_t', as computed by sizeof. */
#define SIZEOF_SIZE_T 8

/* The size of `ssize_t', as computed by sizeof. */
#define SIZEOF_SSIZE_T 8

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Version number of package */
#define VERSION "1.0.5"

/* Define to `short' if <sys/types.h> does not define. */
/* #undef int16_t */

/* Define to `long' if <sys/types.h> does not define. */
/* #undef int32_t */

/* Define to `long long' if <sys/types.h> does not define. */
/* #undef int64_t */

/* Define to `char' if <sys/types.h> does not define. */
/* #undef int8_t */

/* Define to `long long' if <sys/types.h> does not define. */
/* #undef intptr_t */

/* Define to `unsigned long long' if <sys/types.h> does not define. */
/* #undef ptrdiff_t */

/* Define to `unsigned int' if <sys/types.h> does not define. */
/* #undef size_t */

/* Define to `int' if <sys/types.h> does not define. */
/* #undef ssize_t */

/* Define to `unsigned short' if <sys/types.h> does not define. */
/* #undef uint16_t */

/* Define to `unsigned long' if <sys/types.h> does not define. */
/* #undef uint32_t */

/* Define to `unsigned long long' if <sys/types.h> does not define. */
/* #undef uint64_t */

/* Define to `unsigned char' if <sys/types.h> does not define. */
/* #undef uint8_t */

/* Define to `unsigned int' if <sys/types.h> does not define. */
#define uint_t unsigned int

/* Define to `unsigned long long' if <sys/types.h> does not define. */
/* #undef uintptr_t */
