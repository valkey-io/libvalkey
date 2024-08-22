CC?=gcc
CXX?=g++

CFLAGS?=-fPIC -Wall -Wextra -g -O2 -I../include
STLIBNAME?=../lib/libvalkey.a

# Define examples
EXAMPLES=example-blocking example-blocking-push example-async-libevent \
	 example-async-libev example-async-glib example-async-poll \
	 example-cluster-async example-cluster-clientside-caching-async \
	 example-cluster-simple

ifeq ($(USE_SSL),1)
  EXAMPLES+=example-blocking-ssl example-async-libevent-ssl \
	    example-cluster-async-tls example-cluster-tls
  SSL_STLIBNAME=../lib/libvalkey_ssl.a
  SSL_LDFLAGS=-lssl -lcrypto
endif

.PHONY: all clean

all: $(EXAMPLES)

$(STLIBNAME):
	$(MAKE) -C ../

$(SSL_STLIBNAME):
	USE_SSL=1 $(MAKE) -C ../

example-blocking: blocking.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(STLIBNAME)

example-blocking-push: blocking-push.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(STLIBNAME)

example-blocking-ssl: blocking-ssl.c $(STLIBNAME) $(SSL_STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(STLIBNAME) $(SSL_STLIBNAME) $(SSL_LDFLAGS)

example-async-libevent: async-libevent.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -levent $(STLIBNAME)

example-async-libevent-ssl: async-libevent-ssl.c $(STLIBNAME) $(SSL_STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -levent $(STLIBNAME) $(SSL_STLIBNAME) $(SSL_LDFLAGS)

example-async-libev: async-libev.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -lev $(STLIBNAME)

example-async-libhv: async-libhv.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -lhv $(STLIBNAME)

example-async-libsdevent: async-libsdevent.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -lsdevent $(STLIBNAME)

example-async-glib: async-glib.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(shell pkg-config --cflags --libs glib-2.0) $(STLIBNAME)

example-async-ivykis: async-ivykis.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -livykis $(STLIBNAME)

example-async-macosx: async-macosx.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -framework CoreFoundation $(STLIBNAME)

example-async-poll: async-poll.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(STLIBNAME)

ifndef AE_DIR
example-async-ae:
	@echo "Please specify AE_DIR (e.g. <valkey repository>/src)"
	@false
else
example-async-ae: async-ae.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) -I$(AE_DIR) $< $(AE_DIR)/ae.o $(AE_DIR)/zmalloc.o $(AE_DIR)/../deps/jemalloc/lib/libjemalloc.a -pthread $(STLIBNAME)
endif

ifndef LIBUV_DIR
# dynamic link libuv.so
example-async-libuv: async-libuv.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) -I$(LIBUV_DIR)/include $< -luv -lpthread -lrt $(STLIBNAME)
else
# use user provided static lib
example-async-libuv: async-libuv.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) -I$(LIBUV_DIR)/include $< $(LIBUV_DIR)/.libs/libuv.a -lpthread -lrt $(STLIBNAME)
endif

ifeq ($(and $(QT_MOC),$(QT_INCLUDE_DIR),$(QT_LIBRARY_DIR)),)
example-async-qt:
	@echo "Please specify QT_MOC, QT_INCLUDE_DIR AND QT_LIBRARY_DIR"
	@false
else
example-async-qt: async-qt.cpp $(STLIBNAME)
	$(QT_MOC) ../include/valkey/adapters/qt.h | \
	    $(CXX) -x c++ -o qt-adapter-moc.o -c - $(CFLAGS) -I$(QT_INCLUDE_DIR) -I$(QT_INCLUDE_DIR)/QtCore
	$(QT_MOC) async-qt.h | \
	    $(CXX) -x c++ -o qt-example-moc.o -c - $(CFLAGS) -I$(QT_INCLUDE_DIR) -I$(QT_INCLUDE_DIR)/QtCore
	$(CXX) -o $@ $(CFLAGS) $(LDFLAGS) -I$(QT_INCLUDE_DIR) -I$(QT_INCLUDE_DIR)/QtCore -L$(QT_LIBRARY_DIR) qt-adapter-moc.o qt-example-moc.o $< -pthread $(STLIBNAME) -lQt6Core
endif

example-cluster-async: cluster-async.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -levent $(STLIBNAME)

example-cluster-async-tls: cluster-async-tls.c $(STLIBNAME) $(SSL_STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -levent $(STLIBNAME) $(SSL_STLIBNAME) $(SSL_LDFLAGS)

example-cluster-clientside-caching-async: cluster-clientside-caching-async.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -levent $(STLIBNAME)

example-cluster-simple: cluster-simple.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(STLIBNAME)

example-cluster-tls: cluster-tls.c $(STLIBNAME) $(SSL_STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(STLIBNAME) $(SSL_STLIBNAME) $(SSL_LDFLAGS)

clean:
	rm -f example-* *.o
