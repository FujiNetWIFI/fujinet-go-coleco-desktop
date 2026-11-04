# Provide the fujinet-firmware checkout (see cmake/Dependencies.cmake) and
# stage the ColecoVision cart protocol sources into core/coleco/fuji-generated,
# which is what core/CMakeLists.txt compiles into the emulator core.
#
# These are the cartridge firmware's OWN sources, from two roots:
#
#   pico/coleco/firmware   colmap.c / colmap.h (image -> 32K window mapping and
#                      all five ColecoVision mappers: flat, MegaCart,
#                      Activision, X-in-1 and the Opcode SGC), fuji_mailbox.h
#                      (the register and status-page layout) and
#                      baked/fujiconfigrom.h (the baked CONFIG)
#   pico/common        fujimail.c / fujimail.h (hotspot decode, the
#                      REGSEL/REGDATA disarm-after-one-use interlock, ACKSEQ
#                      published last, the DBC push receiver), fujibus.c /
#                      fujibus.h (SLIP + FujiBus wire codec), fuji_cfg.c /
#                      fuji_cfg.h (the .cfg sibling's mapper= line),
#                      fuji_mailbox_proto.h / fuji_mailbox_check.h (the layout
#                      every cart shares) and the emulator-side SLIP-over-TCP
#                      transport (fujitcp)
#
# The firmware's pico/coleco/emu/apply.sh grafts the identical set into a MAME
# tree (minus the baked CONFIG, which MAME takes as its -cart argument); this
# file is that graft for this repository. Emulator and cartridge stay
# identical by construction, not by discipline: nothing here is patched, only
# copied. The one port-specific file, the cart device itself, is this
# repository's own core/coleco/fujinet_cart.c (transposed from the firmware's
# MAME device, pico/coleco/emu/fujinet.cpp).
#
# apply.sh additionally generates a fujins.h that prefixes every shared
# symbol, because fujimail.c/fujibus.c/fujitcp.c are byte-identical across the
# Astrocade, Arcadia and ColecoVision ports but are each compiled against
# their own fuji_mailbox.h -- with two grafted into one MAME binary the linker
# picks a single fujimail_paint() and the others paint the wrong offsets. Only
# one FujiNet cart is ever linked here, so that prefixing is deliberately NOT
# reproduced. Do not add it "for symmetry".
#
# fujitcp.c is POSIX sockets; Windows builds compile the committed Winsock
# twin core/coleco/fujitcp_win32.c against the SAME staged fujitcp.h instead
# (see core/CMakeLists.txt). The staged .c is still copied everywhere so the
# staging result does not depend on the host.
#
# Staging is automatic: it runs when the staged tree is missing, when the pin
# has moved, when this file changes, or on demand with -DFUJI_RESTAGE=ON
# (which is also how to pick up uncommitted edits in a working checkout
# pointed at by FUJINET_SRC).

set(FUJI_GEN "${CMAKE_SOURCE_DIR}/core/coleco/fuji-generated")

option(FUJI_RESTAGE "Re-stage the FujiNet protocol sources from the firmware checkout" OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

coleco_provide_dependency(
  NAME fujinet-firmware
  PATH third_party/fujinet-firmware
  URL "${FUJINET_URL}"
  COMMIT "${FUJINET_COMMIT}"
  SENTINEL build.sh
  OVERRIDE FUJINET_SRC
  RESULT FUJINET_DIR)

set(FUJI_PROTO_DIR "${FUJINET_DIR}/pico")
if(NOT EXISTS "${FUJI_PROTO_DIR}/coleco/firmware/include/fuji_mailbox.h"
   OR NOT EXISTS "${FUJI_PROTO_DIR}/coleco/firmware/baked/fujiconfigrom.h"
   OR NOT EXISTS "${FUJI_PROTO_DIR}/common/src/fujimail.c")
  message(FATAL_ERROR
    "${FUJINET_DIR} has no pico/coleco/firmware/{include/fuji_mailbox.h, "
    "baked/fujiconfigrom.h} or pico/common/src/fujimail.c -- the pin must be "
    "fujinet-firmware master with the ColecoVision assembly CONFIG (#2288); "
    "see cmake/Dependencies.cmake.")
endif()

# The exact file list apply.sh grafts into MAME, relative to pico/, plus the
# baked CONFIG client (the RP2040 serves it when no image is staged -- this
# port's no-cartridge boot does the same; MAME instead takes the client as
# its -cart argument so its graft never needed it).
set(FUJI_STAGE_FILES
  common/src/fujimail.c
  common/src/fujibus.c
  common/src/fuji_cfg.c
  coleco/firmware/src/colmap.c
  common/include/fujimail.h
  common/include/fujibus.h
  common/include/fuji_cfg.h
  coleco/firmware/include/colmap.h
  coleco/firmware/include/fuji_mailbox.h
  common/include/fuji_mailbox_proto.h
  common/include/fuji_mailbox_check.h
  coleco/firmware/baked/fujiconfigrom.h
  common/emu/fujitcp.c
  common/emu/fujitcp.h)

# What the staged tree was made from: the source identity plus a hash of this
# file (the only transform), so that editing the stage re-stages rather than
# silently leaving the old result in place.
set(_fuji_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${FUJINET_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _fuji_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT _fuji_head)
  set(_fuji_head "${FUJINET_COMMIT} ${FUJINET_DIR}")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _fuji_stage_hash)
set(_fuji_want "${_fuji_head} ${_fuji_stage_hash}")

set(_fuji_staged "")
if(EXISTS "${FUJI_GEN}/.source-info")
  file(READ "${FUJI_GEN}/.source-info" _fuji_staged)
  string(STRIP "${_fuji_staged}" _fuji_staged)
endif()

if(FUJI_RESTAGE OR NOT EXISTS "${FUJI_GEN}/fuji_mailbox.h"
   OR NOT _fuji_staged STREQUAL _fuji_want)
  message(STATUS "Staging FujiNet protocol sources from ${FUJI_PROTO_DIR}")
  file(REMOVE_RECURSE "${FUJI_GEN}")
  file(MAKE_DIRECTORY "${FUJI_GEN}")

  foreach(_f IN LISTS FUJI_STAGE_FILES)
    if(NOT EXISTS "${FUJI_PROTO_DIR}/${_f}")
      file(REMOVE_RECURSE "${FUJI_GEN}")
      message(FATAL_ERROR
        "FujiNet protocol staging failed: ${FUJI_PROTO_DIR}/${_f} is missing "
        "(pin drift? see cmake/Dependencies.cmake).")
    endif()
    get_filename_component(_base "${_f}" NAME)
    file(COPY_FILE "${FUJI_PROTO_DIR}/${_f}" "${FUJI_GEN}/${_base}")
  endforeach()

  file(WRITE "${FUJI_GEN}/.source-info" "${_fuji_want}\n")
endif()

message(STATUS "FujiNet protocol sources staged from ${FUJI_PROTO_DIR}")
