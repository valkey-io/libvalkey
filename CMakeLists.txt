CMAKE_MINIMUM_REQUIRED(VERSION 3.0.0)

MACRO(getVersionBit name)
  SET(VERSION_REGEX "^#define ${name} (.+)$")
  FILE(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/valkey/valkey.h"
    VERSION_BIT REGEX ${VERSION_REGEX})
  STRING(REGEX REPLACE ${VERSION_REGEX} "\\1" VERSION_MATCHED "${VERSION_BIT}")
  STRING(REPLACE "\"" "" ${name} "${VERSION_MATCHED}")
ENDMACRO(getVersionBit)

getVersionBit(LIBVALKEY_MAJOR)
getVersionBit(LIBVALKEY_MINOR)
getVersionBit(LIBVALKEY_PATCH)
getVersionBit(LIBVALKEY_SONAME)
SET(VERSION "${LIBVALKEY_MAJOR}.${LIBVALKEY_MINOR}.${LIBVALKEY_PATCH}")
MESSAGE("Detected version: ${VERSION}")

PROJECT(valkey LANGUAGES "C" VERSION "${VERSION}")
INCLUDE(GNUInstallDirs)

OPTION(BUILD_SHARED_LIBS "Build shared libraries" ON)
OPTION(ENABLE_SSL "Build valkey_ssl for SSL support" OFF)
OPTION(DISABLE_TESTS "If tests should be compiled or not" OFF)
OPTION(ENABLE_EXAMPLES "Enable building valkey examples" OFF)
option(ENABLE_IPV6_TESTS "Enable IPv6 tests requiring special prerequisites" OFF)
OPTION(ENABLE_RDMA "Build valkey_rdma for RDMA support" OFF)

# valkey requires C99
SET(CMAKE_C_STANDARD 99)
SET(CMAKE_DEBUG_POSTFIX d)

# Set target-common flags
if(NOT WIN32)
  add_compile_options(-Werror -Wall -Wextra -pedantic)
  add_compile_options(-Wstrict-prototypes -Wwrite-strings -Wno-missing-field-initializers)
else()
  add_definitions(-D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN)
endif()

SET(valkey_sources
    src/adlist.c
    src/alloc.c
    src/async.c
    src/cluster.c
    src/command.c
    src/conn.c
    src/crc16.c
    src/dict.c
    src/net.c
    src/read.c
    src/sds.c
    src/sockcompat.c
    src/valkey.c
    src/vkutil.c)

ADD_LIBRARY(valkey ${valkey_sources})
ADD_LIBRARY(valkey::valkey ALIAS valkey)
set(valkey_export_name valkey CACHE STRING "Name of the exported target")
set_target_properties(valkey PROPERTIES EXPORT_NAME ${valkey_export_name})

SET_TARGET_PROPERTIES(valkey
    PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE
    VERSION "${LIBVALKEY_SONAME}")
IF(MSVC)
    SET_TARGET_PROPERTIES(valkey
        PROPERTIES COMPILE_FLAGS /Z7)
ENDIF()
IF(WIN32)
    TARGET_LINK_LIBRARIES(valkey PUBLIC ws2_32 crypt32)
ELSEIF(CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    TARGET_LINK_LIBRARIES(valkey PUBLIC m)
ELSEIF(CMAKE_SYSTEM_NAME MATCHES "SunOS")
    TARGET_LINK_LIBRARIES(valkey PUBLIC socket)
ENDIF()

TARGET_INCLUDE_DIRECTORIES(valkey
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/valkey>
)

CONFIGURE_FILE(valkey.pc.in valkey.pc @ONLY)

set(CPACK_PACKAGE_VENDOR "Valkey")
set(CPACK_PACKAGE_DESCRIPTION "\
Libvalkey is a minimalistic C client library for the Valkey, KeyDB, and Redis databases

It is minimalistic because it just adds minimal support for the protocol, \
but at the same time it uses a high level printf-alike API in order to make \
it much higher level than otherwise suggested by its minimal code base and the \
lack of explicit bindings for every server command.

Apart from supporting sending commands and receiving replies, it comes with a \
reply parser that is decoupled from the I/O layer. It is a stream parser designed \
for easy reusability, which can for instance be used in higher level language bindings \
for efficient reply parsing.

valkey only supports the binary-safe RESP protocol, so you can use it with any Redis \
compatible server >= 1.2.0.

The library comes with multiple APIs. There is the synchronous API, the asynchronous API \
and the reply parsing API.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/valkey-io/libvalkey")
set(CPACK_PACKAGE_CONTACT "michael dot grunder at gmail dot com")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)

include(CPack)

INSTALL(TARGETS valkey
    EXPORT valkey-targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

if (MSVC AND BUILD_SHARED_LIBS)
    INSTALL(FILES $<TARGET_PDB_FILE:valkey>
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        CONFIGURATIONS Debug RelWithDebInfo)
endif()

install(DIRECTORY include/valkey
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/valkey.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

export(EXPORT valkey-targets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/valkey-targets.cmake"
    NAMESPACE valkey::)

if(WIN32)
    SET(CMAKE_CONF_INSTALL_DIR share/valkey)
else()
    SET(CMAKE_CONF_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/valkey)
endif()
SET(INCLUDE_INSTALL_DIR include)
include(CMakePackageConfigHelpers)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/valkey-config-version.cmake"
                                 COMPATIBILITY SameMajorVersion)
configure_package_config_file(valkey-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/valkey-config.cmake
                              INSTALL_DESTINATION ${CMAKE_CONF_INSTALL_DIR}
                              PATH_VARS INCLUDE_INSTALL_DIR)

INSTALL(EXPORT valkey-targets
        FILE valkey-targets.cmake
        NAMESPACE valkey::
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})

INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/valkey-config.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/valkey-config-version.cmake
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})


IF(ENABLE_SSL)
    IF (NOT OPENSSL_ROOT_DIR)
        IF (APPLE)
            SET(OPENSSL_ROOT_DIR "/usr/local/opt/openssl")
        ENDIF()
    ENDIF()
    FIND_PACKAGE(OpenSSL REQUIRED)
    SET(valkey_ssl_sources
        src/cluster_ssl.c
        src/ssl.c)
    ADD_LIBRARY(valkey_ssl ${valkey_ssl_sources})
    ADD_LIBRARY(valkey::valkey_ssl ALIAS valkey_ssl)

    TARGET_INCLUDE_DIRECTORIES(valkey_ssl
        PRIVATE
            $<INSTALL_INTERFACE:include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/valkey>
    )

    IF (APPLE AND BUILD_SHARED_LIBS)
        SET_PROPERTY(TARGET valkey_ssl PROPERTY LINK_FLAGS "-Wl,-undefined -Wl,dynamic_lookup")
    ENDIF()

    SET_TARGET_PROPERTIES(valkey_ssl
        PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS TRUE
        VERSION "${LIBVALKEY_SONAME}")
    IF(MSVC)
        SET_TARGET_PROPERTIES(valkey_ssl
            PROPERTIES COMPILE_FLAGS /Z7)
    ENDIF()
    TARGET_LINK_LIBRARIES(valkey_ssl PRIVATE OpenSSL::SSL)
    IF(WIN32)
        TARGET_LINK_LIBRARIES(valkey_ssl PRIVATE valkey)
    ENDIF()
    CONFIGURE_FILE(valkey_ssl.pc.in valkey_ssl.pc @ONLY)

    INSTALL(TARGETS valkey_ssl
        EXPORT valkey_ssl-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    if (MSVC AND BUILD_SHARED_LIBS)
        INSTALL(FILES $<TARGET_PDB_FILE:valkey_ssl>
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            CONFIGURATIONS Debug RelWithDebInfo)
    endif()

    INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/valkey_ssl.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

    export(EXPORT valkey_ssl-targets
           FILE "${CMAKE_CURRENT_BINARY_DIR}/valkey_ssl-targets.cmake"
           NAMESPACE valkey::)

    if(WIN32)
        SET(CMAKE_CONF_INSTALL_DIR share/valkey_ssl)
    else()
        SET(CMAKE_CONF_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/valkey_ssl)
    endif()
    configure_package_config_file(valkey_ssl-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/valkey_ssl-config.cmake
                                  INSTALL_DESTINATION ${CMAKE_CONF_INSTALL_DIR}
                                  PATH_VARS INCLUDE_INSTALL_DIR)

    INSTALL(EXPORT valkey_ssl-targets
        FILE valkey_ssl-targets.cmake
        NAMESPACE valkey::
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})

    INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/valkey_ssl-config.cmake
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})
ENDIF()

if(ENABLE_RDMA)
    find_library(RDMACM_LIBRARIES rdmacm)
    find_library(IBVERBS_LIBRARIES ibverbs)
    set(valkey_rdma_sources src/rdma.c)
    add_library(valkey_rdma ${valkey_rdma_sources})
    add_library(valkey::valkey_rdma ALIAS valkey_rdma)

    target_link_libraries(valkey_rdma LINK_PRIVATE ${RDMACM_LIBRARIES} ${IBVERBS_LIBRARIES})
    target_include_directories(valkey_rdma
        PRIVATE
            $<INSTALL_INTERFACE:include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/valkey>
    )

    set_target_properties(valkey_rdma
        PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS TRUE
        VERSION "${LIBVALKEY_SONAME}")
    configure_file(valkey_rdma.pc.in valkey_rdma.pc @ONLY)

    install(TARGETS valkey_rdma
        EXPORT valkey_rdma-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/valkey_rdma.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

    export(EXPORT valkey_rdma-targets
           FILE "${CMAKE_CURRENT_BINARY_DIR}/valkey_rdma-targets.cmake"
           NAMESPACE valkey::)

    set(CMAKE_CONF_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/valkey_rdma)
    configure_package_config_file(valkey_rdma-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/valkey_rdma-config.cmake
                                  INSTALL_DESTINATION ${CMAKE_CONF_INSTALL_DIR}
                                  PATH_VARS INCLUDE_INSTALL_DIR)

    install(EXPORT valkey_rdma-targets
        FILE valkey_rdma-targets.cmake
        NAMESPACE valkey::
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/valkey_rdma-config.cmake
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})
endif()

# Add tests
if(NOT DISABLE_TESTS)
  # Make sure ctest prints the output when a test fails.
  # Must be set before including CTest.
  set(CMAKE_CTEST_ARGUMENTS "--output-on-failure")
  include(CTest)
  add_subdirectory(tests)
endif()

# Add examples
IF(ENABLE_EXAMPLES)
    ADD_SUBDIRECTORY(examples)
ENDIF(ENABLE_EXAMPLES)
