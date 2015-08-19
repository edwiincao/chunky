AC_INIT(chunky, 0.1.0)
AC_CONFIG_SRCDIR([.])
AM_INIT_AUTOMAKE([no-define foreign])

AC_CONFIG_MACRO_DIR([m4])
m4_include([m4/ax_cxx_compile_stdcxx_11.m4])
m4_include([m4/ax_pthread.m4])
m4_include([m4/boost.m4])
m4_include([m4/libcurl.m4])
m4_include([m4/ax_check_openssl.m4])

AC_PROG_CXX
AC_PROG_LIBTOOL

AX_CXX_COMPILE_STDCXX_11(,mandatory)
AC_LANG(C++)

AC_DEFINE([NDEBUG])

# std::thread requires pthread on Linux.
AX_PTHREAD([true])
AM_CONDITIONAL([NEEDS_PTHREAD], [echo $host_os | grep -q "linux"])

# Library dependencies
#
# chunky itself requires only Boost headers and the Boost System
# library (for Boost Asio). OpenSSL is optional if chunky is used with
# Boost Asio SSL classes, plus the websocket sample also uses OpenSSL.
# libcurl is needed for the unit test.
BOOST_REQUIRE([1.54], [AC_MSG_ERROR([missing or obsolete Boost])])
BOOST_DATE_TIME
BOOST_LOG
BOOST_SYSTEM
BOOST_TEST

LIBCURL_CHECK_CONFIG(,,, [AC_MSG_WARN('make check' requires libcurl)])
AM_CONDITIONAL([HAS_LIBCURL], [test -n "LIBCURL"])

AX_CHECK_OPENSSL(, [AC_MSG_WARN(['make check' and some samples require OpenSSL])])
AM_CONDITIONAL([HAS_OPENSSL], [test -n "$OPENSSL_LIBS"])

AC_OUTPUT(Makefile)
