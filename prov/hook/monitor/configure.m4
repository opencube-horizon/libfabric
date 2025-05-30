dnl Configury specific to the libfabrics monitor hooking provider

dnl Called to configure this provider
dnl
dnl Arguments:
dnl
dnl $1: action if configured successfully
dnl $2: action if not configured successfully
dnl

AC_DEFUN([FI_MONITOR_CONFIGURE],[
    # Determine if we can support the monitor hooking provider
    monitor_happy=0
    AS_IF([test x"$enable_monitor" != x"no"], [monitor_happy=1])
    AS_IF([test $monitor_happy -eq 1], [$1], [$2])
])
