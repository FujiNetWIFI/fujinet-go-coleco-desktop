# Provide the adamcore sources (see cmake/Dependencies.cmake) and stage them
# into core/adamcore-generated, which is what core/CMakeLists.txt compiles as
# if they were ours -- the same arrangement fujinet-go-adam-desktop uses, so
# the core builds with this project's own flags and warning settings.
#
# Nothing is patched, only copied. The ColecoVision-side work this app needs
# (the adamcore_cart_ops vtable, the Opcode Super Game Module, the corrected
# stock-console memory map) lands UPSTREAM in adamcore rather than as a patch
# script here: it is the same maintainer's repository, and the SGM benefits
# fujinet-go-adam-desktop too. Develop the two in tandem with
#
#     cmake -B build -DADAMCORE_SRC=~/Workspace/adamcore -DADAMCORE_RESTAGE=ON
#
# The copy is done in CMake rather than by a shell script: MSYS2/MinGW
# configures cannot execute a .sh through execute_process ("inappropriate
# file type or format"), and staging is a directory copy plus a marker file.
#
# Staging is automatic: it runs when the staged tree is missing, when the
# source checkout has moved to a different commit, when this file changes, or
# on demand with -DADAMCORE_RESTAGE=ON (which is also how to pick up
# uncommitted edits in a working checkout pointed at by ADAMCORE_SRC).

set(ADAMCORE_GEN "${CMAKE_SOURCE_DIR}/core/adamcore-generated")

option(ADAMCORE_RESTAGE "Re-stage adamcore sources from the source checkout" OFF)

coleco_provide_dependency(
  NAME adamcore
  PATH third_party/adamcore
  URL "${ADAMCORE_URL}"
  COMMIT "${ADAMCORE_COMMIT}"
  SENTINEL src/machine.c
  OVERRIDE ADAMCORE_SRC
  RESULT ADAMCORE_DIR)

# What the staged tree was made from: the source identity plus a hash of this
# file (the only transform), so editing the stage re-stages rather than
# silently leaving the old result in place.
set(_adamcore_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${ADAMCORE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _adamcore_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT _adamcore_head)
  set(_adamcore_head "${ADAMCORE_DIR}")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _adamcore_stage_hash)
set(_adamcore_want "${_adamcore_head} ${_adamcore_stage_hash}")

set(_adamcore_staged "")
if(EXISTS "${ADAMCORE_GEN}/.source-info")
  file(READ "${ADAMCORE_GEN}/.source-info" _adamcore_staged)
  string(STRIP "${_adamcore_staged}" _adamcore_staged)
endif()

if(ADAMCORE_RESTAGE OR NOT EXISTS "${ADAMCORE_GEN}/src/machine.c"
   OR NOT _adamcore_staged STREQUAL _adamcore_want)
  message(STATUS "Staging adamcore sources from ${ADAMCORE_DIR}")
  file(REMOVE_RECURSE "${ADAMCORE_GEN}")
  file(MAKE_DIRECTORY "${ADAMCORE_GEN}")
  file(COPY "${ADAMCORE_DIR}/include" "${ADAMCORE_DIR}/src"
       DESTINATION "${ADAMCORE_GEN}")
  if(NOT EXISTS "${ADAMCORE_GEN}/src/machine.c")
    message(FATAL_ERROR "adamcore staging failed (source: ${ADAMCORE_DIR})")
  endif()
  file(WRITE "${ADAMCORE_GEN}/.source-info" "${_adamcore_want}\n")
endif()

# The ColecoVision cartridge-device vtable is what the FujiNet mailbox cart
# (core/coleco/fujinet_cart.c) plugs into. It lands upstream in adamcore as
# part of M1; until the pin carries it, say so plainly here rather than
# letting core/ fail with a compile error about an unknown type.
if(NOT EXISTS "${ADAMCORE_GEN}/include/adamcore.h")
  message(FATAL_ERROR "adamcore staging produced no include/adamcore.h")
endif()
file(READ "${ADAMCORE_GEN}/include/adamcore.h" _adamcore_hdr)
string(FIND "${_adamcore_hdr}" "adamcore_cart_ops" _adamcore_has_cart_ops)
if(_adamcore_has_cart_ops EQUAL -1)
  set(COLECO_ADAMCORE_HAS_CART_OPS OFF)
  message(STATUS
    "adamcore: no adamcore_cart_ops in this checkout -- the FujiNet cart "
    "device is disabled. See Part A of the port plan; develop against a "
    "working checkout with -DADAMCORE_SRC=... -DADAMCORE_RESTAGE=ON.")
else()
  set(COLECO_ADAMCORE_HAS_CART_OPS ON)
endif()

message(STATUS "adamcore staged at ${_adamcore_head}")
