# Makefile for tools/netctl
#
# Builds two binaries:
#   ff_netd    – network-configuration daemon (DPDK secondary process)
#   ff_netctl  – CLI client (pure Linux, no DPDK)
#
# ff_netd is compiled with the same FreeBSD-compat + DPDK flags used by the
# other F-Stack tools so that ioctl()/sysctl() are transparently routed
# through the IPC rings.  ff_netctl needs only the standard libc.

TOPDIR?=${CURDIR}/../..

ifneq ($(shell pkg-config --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found – export PKG_CONFIG_PATH if needed")
endif

include ${TOPDIR}/tools/opts.mk

PKGCONF  ?= pkg-config
PROGDIR   = ${TOPDIR}/tools/sbin

# -----------------------------------------------------------------------
# Compiler flags for ff_netd (mirrors tools/prog.mk)
# -----------------------------------------------------------------------
FF_DAEMON_CFLAGS  := -g -Wall -Werror -Wno-deprecated-declarations
FF_DAEMON_CFLAGS  += -DFSTACK -std=gnu99
FF_DAEMON_CFLAGS  += $(shell $(PKGCONF) --cflags libdpdk)
FF_DAEMON_CFLAGS  += -I${TOPDIR}/lib
FF_DAEMON_CFLAGS  += -I${TOPDIR}/tools/compat
FF_DAEMON_CFLAGS  += -I${TOPDIR}/tools/compat/include
FF_DAEMON_CFLAGS  += -include${TOPDIR}/tools/compat/compat.h
FF_DAEMON_CFLAGS  += -D__BSD_VISIBLE -DINET6 -DINET
FF_DAEMON_CFLAGS  += -Wno-unused-but-set-variable -Wno-unused-variable
FF_DAEMON_CFLAGS  += -Wno-sign-compare

FF_DAEMON_LIBS    := -L${TOPDIR}/tools/compat
FF_DAEMON_LIBS    += -Wl,--whole-archive,-lffcompat,--no-whole-archive
FF_DAEMON_LIBS    += $(shell $(PKGCONF) --static --libs libdpdk)
FF_DAEMON_LIBS    += -Wl,--no-whole-archive
FF_DAEMON_LIBS    += -lrt -lm -ldl -lcrypto -lpthread -lnuma

# -----------------------------------------------------------------------
# Compiler flags for ff_netctl (no DPDK / compat headers)
# -----------------------------------------------------------------------
FF_CTL_CFLAGS     := -g -Wall -Werror -std=gnu99

# -----------------------------------------------------------------------
# Build targets
# -----------------------------------------------------------------------
all: ${PROGDIR}/ff_netd ${PROGDIR}/ff_netctl

${PROGDIR}/ff_netd: ff_netd.o
	${CC} -o $@ $< ${FF_DAEMON_LIBS}

ff_netd.o: ff_netd.c
	${CC} -c $< ${FF_DAEMON_CFLAGS} -o $@

${PROGDIR}/ff_netctl: ff_netctl.o
	${CC} -o $@ $<

ff_netctl.o: ff_netctl.c
	${CC} -c $< ${FF_CTL_CFLAGS} -o $@

.PHONY: clean
clean:
	rm -f *.o ${PROGDIR}/ff_netd ${PROGDIR}/ff_netctl
