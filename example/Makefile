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

TARGET="helloworld"
all: main main_epoll main_proxy main_epoll_fstack main_epoll_fstack_v2

main: main.c
	cc ${CFLAGS} -DINET6 -o ${TARGET} main.c ${LIBS}

main_epoll: main_epoll.c
	cc ${CFLAGS} -o ${TARGET}_epoll main_epoll.c ${LIBS}

main_proxy: main_proxy.c
	cc ${CFLAGS} -o ${TARGET}_proxy main_proxy.c ${LIBS}

main_epoll_fstack: main_epoll_fstack.cpp
	g++ ${CFLAGS} -o ${TARGET}_epoll_fstack main_epoll_fstack.cpp ${LIBS} -DFSTACK_ENABLE_BACKEND

main_epoll_fstack_v2: main_epoll_fstack_v2.cpp
	g++ ${CFLAGS} -o ${TARGET}_epoll_fstack_v2 main_epoll_fstack_v2.cpp ${LIBS} -DFSTACK_ENABLE_BACKEND

.PHONY: clean
clean:
	rm -f *.o ${TARGET} ${TARGET}_epoll ${TARGET}_proxy ${TARGET}_epoll_fstack ${TARGET}_epoll_fstack_v2
