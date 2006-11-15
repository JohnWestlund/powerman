##*****************************************************************************
## $Id: ac_ncurses.m4,v 1.1.1.1 2003/09/05 16:05:42 achu Exp $
##*****************************************************************************
#  AUTHOR:
#    Albert Chu  <chu11@llnl.gov>
#
#  SYNOPSIS:
#    AC_WRAP
#
#  DESCRIPTION:
#    Check for wrap library
#
#  WARNINGS:
#    This macro must be placed after AC_PROG_CC or equivalent.
##*****************************************************************************

AC_DEFUN([AC_WRAP],
[
  AC_CHECK_LIB([wrap], [hosts_ctl], [ac_have_wrap=yes], [ac_have_wrap=no])

  if test "$ac_have_wrap" = "yes"; then
    LIBWRAP="-lwrap"
  else
    AC_MSG_ERROR([libwrap required for this package])
  fi        

  AC_SUBST(LIBWRAP)
])
