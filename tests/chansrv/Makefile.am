AM_CPPFLAGS = \
  -I$(top_builddir) \
  -I$(top_srcdir)/sesman/chansrv \
  -I$(top_srcdir)/common

LOG_DRIVER = env AM_TAP_AWK='$(AWK)' $(SHELL) \
                  $(top_srcdir)/tap-driver.sh

PACKAGE_STRING = "chansrv"

TESTS = test_chansrv
check_PROGRAMS = test_chansrv

test_chansrv_SOURCES = \
    test_chansrv_main.c \
    test_sound_latency.c \
    $(top_srcdir)/sesman/chansrv/sound_latency.c

test_chansrv_CFLAGS = \
    @CHECK_CFLAGS@

test_chansrv_LDADD = \
    @CHECK_LIBS@
