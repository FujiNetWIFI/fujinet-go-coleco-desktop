# External source dependencies.
#
# Standard practice for every target in the FujiNet Go family: the build
# provides each dependency for itself. A plain `git clone` of this repository
# (no --recurse-submodules), GNOME Builder, a flatpak build or a source
# tarball all end up with a usable checkout without the developer having to
# know the dependency exists.
#
# Two dependencies:
#
#   adamcore          the clean-room Coleco ADAM / ColecoVision emulator core
#                     (GPL-3.0-or-later). Staged into core/adamcore-generated
#                     by cmake/StageAdamcore.cmake and compiled as if it were
#                     ours, the same arrangement fujinet-go-adam-desktop uses.
#
#   fujinet-firmware  Unlike most sibling desktop apps this pin is NOT on
#                     master: the ColecoVision cart protocol (pico/coleco --
#                     the mailbox spec in fuji_mailbox.h, the shared
#                     fujibus/fujimail/colmap sources, and the MAME cart
#                     device core/coleco/fujinet_cart.c is transposed from)
#                     lives on the colecovision-bringup branch, pending its
#                     merge into master. Moving to master later is a pin bump
#                     here and in both flatpak manifests, nothing more.
#
# Resolution order:
#   1. ADAMCORE_SRC / FUJINET_SRC (cache variable or environment) -- an
#      out-of-tree working checkout, which is how these are developed in
#      tandem, e.g.
#          cmake -B build -DADAMCORE_SRC=~/Workspace/adamcore \
#                         -DFUJINET_SRC=~/Workspace/fujinet-firmware
#   2. third_party/<name>, initialising the submodule when this tree is a git
#      checkout.
#   3. No git metadata to work from (release tarballs, some IDE source
#      copies): a direct clone of the pinned commit.

find_package(Git QUIET)

# adamcore, master (2fd4ff058). The ColecoVision half of this core -- the
# cartridge-device vtable (adamcore_cart_ops), the Opcode Super Game Module
# and the corrected stock-console memory map -- is developed here in tandem
# via ADAMCORE_SRC and lands upstream; bump this pin as each piece merges.
set(ADAMCORE_COMMIT "2fd4ff058e65bd181f724e853e1b81011e060577")
set(ADAMCORE_URL "https://github.com/tschak909/adamcore")

# fujinet-firmware, branch colecovision-bringup (844eb6f24). Contains the
# complete pico/coleco port: the mailbox protocol, all five cartridge mappers
# in colmap.c, the baked-in CONFIG client, the MAME cart device, and the .col
# media-type registration without which no cartridge in the No-Intro set
# reaches MediaTypeROM. Recorded in three places -- here and in both flatpak
# manifests (build-aux/flatpak/*.yml). The pinned commit has moved on the
# server before in a sibling repo, so treat a failed fetch of this exact hash
# as "the branch was rebased/merged", not as a network fault.
set(FUJINET_COMMIT "844eb6f244e1d1cba3dacd1c0f78a5ab3d47f4ab")
set(FUJINET_URL "https://github.com/FujiNetWIFI/fujinet-firmware")

# coleco_provide_dependency(NAME <n> PATH <p> SENTINEL <file> OVERRIDE <VAR>
#                           RESULT <out>
#                           [URL <u> COMMIT <sha>]        # git dependency
#                           [ARCHIVE <url> SHA256 <hash> [STRIP <dir>]])
#
# SENTINEL is a path inside the checkout that only exists once the sources are
# really there -- an empty submodule directory is otherwise indistinguishable
# from a populated one, and a half-extracted archive from a complete one.
function(coleco_provide_dependency)
  cmake_parse_arguments(DEP ""
    "NAME;PATH;URL;COMMIT;ARCHIVE;SHA256;STRIP;SENTINEL;OVERRIDE;RESULT" ""
    ${ARGN})

  # 1. Explicit override: a checkout the developer maintains themselves.
  set(_override "")
  if(DEFINED ${DEP_OVERRIDE})
    set(_override "${${DEP_OVERRIDE}}")
  elseif(DEFINED ENV{${DEP_OVERRIDE}})
    set(_override "$ENV{${DEP_OVERRIDE}}")
  endif()
  if(_override)
    if(NOT EXISTS "${_override}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "${DEP_OVERRIDE}=${_override} does not look like a ${DEP_NAME} "
        "checkout (no ${DEP_SENTINEL}).")
    endif()
    message(STATUS "${DEP_NAME}: using ${_override} (${DEP_OVERRIDE})")
    set(${DEP_RESULT} "${_override}" PARENT_SCOPE)
    return()
  endif()

  set(_path "${CMAKE_SOURCE_DIR}/${DEP_PATH}")

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}" AND DEP_ARCHIVE)
    # 2a. Archive dependency: download the pin and unpack it.
    #
    # Extract into a scratch directory first and move the result into place
    # only once it is complete, so an interrupted configure cannot leave a
    # partial tree that the sentinel test would go on to accept.
    set(_dl "${CMAKE_BINARY_DIR}/_deps/${DEP_NAME}.archive")
    set(_tmp "${CMAKE_BINARY_DIR}/_deps/${DEP_NAME}-extract")

    if(NOT EXISTS "${_dl}")
      message(STATUS "${DEP_NAME}: downloading ${DEP_ARCHIVE}")
      file(DOWNLOAD "${DEP_ARCHIVE}" "${_dl}"
           EXPECTED_HASH SHA256=${DEP_SHA256}
           TLS_VERIFY ON
           STATUS _dl_status)
      list(GET _dl_status 0 _dl_rc)
      if(NOT _dl_rc EQUAL 0)
        list(GET _dl_status 1 _dl_msg)
        file(REMOVE "${_dl}")
        message(FATAL_ERROR
          "Could not download ${DEP_NAME} from ${DEP_ARCHIVE}: ${_dl_msg}\n"
          "Download it by hand and unpack it, then point ${DEP_OVERRIDE} at "
          "the result.")
      endif()
    endif()

    file(REMOVE_RECURSE "${_tmp}")
    file(MAKE_DIRECTORY "${_tmp}")
    file(ARCHIVE_EXTRACT INPUT "${_dl}" DESTINATION "${_tmp}")

    set(_extracted "${_tmp}")
    if(DEP_STRIP)
      set(_extracted "${_tmp}/${DEP_STRIP}")
    endif()
    if(NOT EXISTS "${_extracted}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "${DEP_NAME} archive did not contain ${DEP_STRIP}/${DEP_SENTINEL} "
        "(${DEP_ARCHIVE}).")
    endif()

    file(REMOVE_RECURSE "${_path}")
    get_filename_component(_parent "${_path}" DIRECTORY)
    file(MAKE_DIRECTORY "${_parent}")
    file(RENAME "${_extracted}" "${_path}")
    file(REMOVE_RECURSE "${_tmp}")

  elseif(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
      message(FATAL_ERROR
        "${DEP_NAME} is missing and git is not installed. Either install git "
        "or unpack ${DEP_URL} (commit ${DEP_COMMIT}) into ${DEP_PATH}.")
    endif()

    if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
      # 2b. Submodule checkout. --filter=blob:none keeps the fetch to the
      # history the build needs; fujinet-firmware is a large repository.
      message(STATUS "${DEP_NAME}: fetching submodule ${DEP_PATH}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} submodule update --init --filter=blob:none
                -- "${DEP_PATH}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _rc)
      if(NOT _rc EQUAL 0)
        # Older servers/mirrors may refuse partial clones.
        execute_process(
          COMMAND ${GIT_EXECUTABLE} submodule update --init -- "${DEP_PATH}"
          WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
          RESULT_VARIABLE _rc)
      endif()
    endif()

    if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
      # 3. No git metadata (tarball): clone the pin outright.
      message(STATUS "${DEP_NAME}: cloning ${DEP_URL} @ ${DEP_COMMIT}")
      file(REMOVE_RECURSE "${_path}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} clone --filter=blob:none "${DEP_URL}" "${_path}"
        RESULT_VARIABLE _rc)
      if(_rc EQUAL 0)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -c advice.detachedHead=false checkout
                  --quiet "${DEP_COMMIT}"
          WORKING_DIRECTORY "${_path}"
          RESULT_VARIABLE _rc)
      endif()
    endif()
  endif()

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    if(DEP_ARCHIVE)
      message(FATAL_ERROR
        "Could not provide ${DEP_NAME}. Unpack ${DEP_ARCHIVE} into "
        "${DEP_PATH}, or point ${DEP_OVERRIDE} at an existing checkout.")
    else()
      message(FATAL_ERROR
        "Could not provide ${DEP_NAME}. Fetch it manually with\n"
        "    git submodule update --init ${DEP_PATH}\n"
        "or point ${DEP_OVERRIDE} at an existing checkout.")
    endif()
  endif()

  # Warn when a git checkout has drifted from the pin recorded here -- the
  # staged protocol sources are the wire format, and a drifted tree can
  # silently change what the cart device speaks.
  if(DEP_COMMIT AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
      execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${_path}" rev-parse HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc)
      if(_rc EQUAL 0 AND NOT _head STREQUAL DEP_COMMIT)
        message(STATUS
          "${DEP_NAME}: checkout is ${_head}, pinned ${DEP_COMMIT} "
          "(cmake/Dependencies.cmake)")
      endif()
    endif()
  endif()

  message(STATUS "${DEP_NAME}: ${_path}")
  set(${DEP_RESULT} "${_path}" PARENT_SCOPE)
endfunction()
