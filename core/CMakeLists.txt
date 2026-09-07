# coleco_core: the toolkit-agnostic, SDL-free ColecoVision core.
#
# Two staged trees, neither of them ours and neither of them patched:
#
#   adamcore-generated/   the emulator (Z80, TMS9928A, SN76489, the
#                         ColecoVision I/O map), staged by
#                         cmake/StageAdamcore.cmake and compiled here as if it
#                         were ours so it gets this project's flags.
#
#   coleco/fuji-generated/  the cartridge firmware's OWN protocol sources --
#                         fujimail.c (the mailbox), fujibus.c (the SLIP +
#                         FujiBus wire codec), colmap.c (every ColecoVision
#                         mapper) and the emulator-side fujitcp transport --
#                         staged by cmake/StageFujiProto.cmake. Compiling
#                         these rather than reimplementing them is what makes
#                         the emulated cartridge and the real RP2040 one
#                         identical by construction.
#
# The one port-specific file is core/coleco/fujinet_cart.c: the cart device
# itself, transposed from the firmware's MAME device
# (pico/coleco/emu/fujinet.cpp). It plugs into adamcore through the
# adamcore_cart_ops vtable.
#
# Nothing here links SDL. The session layer (SDL3 audio + gamepad) is a
# separate target so that stays true as this grows; no_sdl_link guards it.

set(ADAMCORE_GEN "${CMAKE_SOURCE_DIR}/core/adamcore-generated")
set(FUJI_GEN "${CMAKE_SOURCE_DIR}/core/coleco/fuji-generated")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(NOT EXISTS "${FUJI_GEN}/fuji_mailbox.h")
  message(FATAL_ERROR
    "Staged FujiNet protocol sources are missing. cmake/StageFujiProto.cmake "
    "should have produced them; re-run with -DFUJI_RESTAGE=ON.")
endif()

# The staged adamcore translation units.
#
# adamcore ships the list itself, so a source added upstream cannot silently
# break the link here -- which it would if this file enumerated them by hand,
# as the sibling desktop apps used to. adamcore_sources() also picks the right
# socket backend for the platform.
include("${ADAMCORE_GEN}/adamcore-sources.cmake")
adamcore_sources(COLECO_ADAMCORE_SOURCES ROOT "${ADAMCORE_GEN}")

# The staged protocol sources. fujitcp is the emulator-side transport: the
# staged .c is POSIX sockets, so a Windows build compiles the committed
# Winsock twin against the same staged fujitcp.h instead.
set(COLECO_FUJI_SOURCES
  ${FUJI_GEN}/fujimail.c
  ${FUJI_GEN}/fujibus.c
  ${FUJI_GEN}/colmap.c
)
if(WIN32)
  list(APPEND COLECO_FUJI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/coleco/fujitcp_win32.c")
else()
  list(APPEND COLECO_FUJI_SOURCES "${FUJI_GEN}/fujitcp.c")
endif()

set(COLECO_CORE_SOURCES ${COLECO_ADAMCORE_SOURCES} ${COLECO_FUJI_SOURCES})

# The cart device needs adamcore's cartridge vtable, which lands upstream as
# part of M1. Until the pin carries it the core still builds -- a plain
# ColecoVision with no FujiNet -- rather than failing the configure.
if(NOT COLECO_ADAMCORE_HAS_CART_OPS)
  set(COLECO_HAVE_CART OFF)
  message(STATUS "coleco_core: FujiNet cart device not built -- the pinned "
                 "adamcore has no adamcore_cart_ops")
elseif(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/coleco/fujinet_cart.c")
  set(COLECO_HAVE_CART OFF)
  message(STATUS "coleco_core: FujiNet cart device not written yet (M2)")
else()
  list(APPEND COLECO_CORE_SOURCES coleco/fujinet_cart.c)
  set(COLECO_HAVE_CART ON)
endif()

add_library(coleco_core STATIC ${COLECO_CORE_SOURCES})
set_target_properties(coleco_core PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(coleco_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/coleco"
  "${ADAMCORE_GEN}/include"
  # public because fujinet_cart.h -- part of the core's own API surface --
  # includes the staged colmap.h and fuji_mailbox.h.
  "${FUJI_GEN}")
target_include_directories(coleco_core PRIVATE "${ADAMCORE_GEN}/src")

# fujitcp.c uses getaddrinfo/gettimeofday -- POSIX, behind _DEFAULT_SOURCE
# under a strict -std=c11. adamcore's own sources want _GNU_SOURCE.
target_compile_definitions(coleco_core PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)
if(COLECO_HAVE_CART)
  target_compile_definitions(coleco_core PUBLIC COLECO_HAVE_FUJINET_CART=1)
endif()

target_link_libraries(coleco_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(coleco_core PUBLIC ws2_32)
else()
  target_link_libraries(coleco_core PUBLIC m)
endif()

# Our own modules are warning-clean; the staged trees are vendored and are
# built with whatever warnings upstream is comfortable with.
if(COLECO_HAVE_CART)
  set_source_files_properties(coleco/fujinet_cart.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
endif()

add_subdirectory(tests)
