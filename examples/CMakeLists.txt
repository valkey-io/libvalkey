find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(GLIB2 glib-2.0)
  if(GLIB2_FOUND)
    INCLUDE_DIRECTORIES(${GLIB2_INCLUDE_DIRS})
    LINK_DIRECTORIES(${GLIB2_LIBRARY_DIRS})
    ADD_EXECUTABLE(async-glib async-glib.c)
    TARGET_LINK_LIBRARIES(async-glib valkey ${GLIB2_LIBRARIES})
  endif()
endif()

FIND_PATH(LIBEV ev.h
    HINTS /usr/local /usr/opt/local
    ENV LIBEV_INCLUDE_DIR)

if (LIBEV)
    # Just compile and link with libev
    ADD_EXECUTABLE(async-libev async-libev.c)
    TARGET_LINK_LIBRARIES(async-libev valkey ev)
ENDIF()

FIND_PATH(LIBEVENT event.h)
if (LIBEVENT)
    ADD_EXECUTABLE(async-libevent async-libevent.c)
    TARGET_LINK_LIBRARIES(async-libevent valkey event)
ENDIF()

FIND_PATH(LIBHV hv/hv.h)
IF (LIBHV)
    ADD_EXECUTABLE(async-libhv async-libhv.c)
    TARGET_LINK_LIBRARIES(async-libhv valkey hv)
ENDIF()

FIND_PATH(LIBUV uv.h)
IF (LIBUV)
    ADD_EXECUTABLE(async-libuv async-libuv.c)
    TARGET_LINK_LIBRARIES(async-libuv valkey uv)
ENDIF()

FIND_PATH(LIBSDEVENT systemd/sd-event.h)
IF (LIBSDEVENT)
    ADD_EXECUTABLE(async-libsdevent async-libsdevent.c)
    TARGET_LINK_LIBRARIES(async-libsdevent valkey systemd)
ENDIF()

IF (APPLE)
    FIND_LIBRARY(CF CoreFoundation)
    ADD_EXECUTABLE(async-macosx async-macosx.c)
    TARGET_LINK_LIBRARIES(async-macosx valkey ${CF})
ENDIF()

IF (ENABLE_SSL)
    ADD_EXECUTABLE(example-ssl example-ssl.c)
    TARGET_LINK_LIBRARIES(example-ssl valkey valkey_ssl)
ENDIF()

ADD_EXECUTABLE(example blocking.c)
TARGET_LINK_LIBRARIES(example valkey)

ADD_EXECUTABLE(blocking-push blocking-push.c)
TARGET_LINK_LIBRARIES(blocking-push valkey)
