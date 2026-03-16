TOPDIR=..

ifeq ($(FF_PATH),)
	FF_PATH=${TOPDIR}
endif

ifneq ($(shell pkg-config --exists libdpdk && echo 0),0)
$(error "No installation of DPDK found, maybe you should export environment variable `PKG_CONFIG_PATH`")
endif

PKGCONF ?= pkg-config

CFLAGS += -O0 -g -gdwarf-2 $(shell $(PKGCONF) --cflags libdpdk) -I${FF_PATH}/include

LIBS+= $(shell $(PKGCONF) --static --libs libdpdk)
LIBS+= -L${FF_PATH}/lib -Wl,--whole-archive,-lfstack,--no-whole-archive
LIBS+= -Wl,--no-whole-archive -lrt -lm -ldl -lcrypto -pthread -lnuma

SRCS = main.c main_epoll.c main_proxy.c main_epoll_fstack.cpp

TARGET="helloworld"
all: ${SRCS}
	cc ${CFLAGS} -DINET6 -o ${TARGET} main.c ${LIBS}
	cc ${CFLAGS} -o ${TARGET}_epoll main_epoll.c ${LIBS}
	cc ${CFLAGS} -o ${TARGET}_proxy main_proxy.c ${LIBS}
	g++ ${CFLAGS} -o ${TARGET}_epoll_fstack main_epoll_fstack.cpp ${LIBS} -DFSTACK_ENABLE_BACKEND

.PHONY: clean
clean:
	rm -f *.o ${TARGET} ${TARGET}_epoll ${TARGET}_proxy ${TARGET}_epoll_fstack
