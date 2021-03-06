dnl A macro to check presence of a cwrap wrapper on the system
dnl Usage:
dnl     AM_CHECK_WRAPPER(name, conditional)
dnl If the cwrap library is found, sets the HAVE_$name conditional
AC_DEFUN([AM_CHECK_WRAPPER],
[
    FOUND_WRAPPER=0

    AC_MSG_CHECKING([for $1])
    PKG_CHECK_EXISTS([$1],
                     [
                        AC_MSG_RESULT([yes])
                        FOUND_WRAPPER=1
                     ],
                     [
                        AC_MSG_RESULT([no])
                        AC_MSG_WARN([cwrap library $1 not found, some tests will not run])
                     ])

    AM_CONDITIONAL($2, [ test x$FOUND_WRAPPER = x1])
])

AC_DEFUN([AM_CHECK_UID_WRAPPER],
[
    AM_CHECK_WRAPPER(uid_wrapper, HAVE_UID_WRAPPER)
])

AC_DEFUN([AM_CHECK_NSS_WRAPPER],
[
    AM_CHECK_WRAPPER(nss_wrapper, HAVE_NSS_WRAPPER)
])
