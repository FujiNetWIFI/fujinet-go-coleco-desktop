# The core's own tests. Everything here links coleco_core alone -- no SDL, no
# frontend toolkit -- so the whole suite runs on a headless machine and in a
# build sandbox.

add_executable(no_sdl_link_test no_sdl_link_test.c)
target_link_libraries(no_sdl_link_test coleco_core)
add_test(NAME no_sdl_link COMMAND no_sdl_link_test)

# The FujiNet cartridge device, driven through adamcore's bus. Only built
# when the device is (it needs adamcore_cart_ops).
if(COLECO_HAVE_CART)
  add_executable(cart_test cart_test.c)
  target_link_libraries(cart_test coleco_core)
  target_include_directories(cart_test PRIVATE "${CMAKE_SOURCE_DIR}/core/debugger")
  add_test(NAME cart COMMAND cart_test)
endif()

# The debugger's FujiNet trace decoder, fed synthetic SLIP bytes. No ROM, no
# network, no SDL: it always runs.
add_executable(fujitrace_test fujitrace_test.c)
target_link_libraries(fujitrace_test coleco_core)
target_include_directories(fujitrace_test PRIVATE "${CMAKE_SOURCE_DIR}/core/debugger")
add_test(NAME fujitrace COMMAND fujitrace_test)

# ---- ENGINE agent: the debugger engine --------------------------------------
# expr_test, memtools_test and prompt_test need no machine. engine_test starts
# one on a hand-assembled 8K "BIOS" and needs no ROM either, so none of them
# SKIP. It is the machine-boot test the Wine and macOS runners cannot run: it is
# in the ctest -E lists of the workflows (see the CI notes).
add_executable(expr_test expr_test.c)
target_link_libraries(expr_test coleco_core)
target_include_directories(expr_test PRIVATE "${CMAKE_SOURCE_DIR}/core/debugger")
add_test(NAME expr COMMAND expr_test)

add_executable(memtools_test memtools_test.c)
target_link_libraries(memtools_test coleco_core)
add_test(NAME memtools COMMAND memtools_test)

add_executable(prompt_test prompt_test.c)
target_link_libraries(prompt_test coleco_core)
add_test(NAME prompt COMMAND prompt_test)

add_executable(engine_test engine_test.c)
target_link_libraries(engine_test coleco_core)
add_test(NAME engine COMMAND engine_test)
set_tests_properties(engine PROPERTIES TIMEOUT 120)
# ---- end ENGINE agent --------------------------------------------------------

# ---- VIDEO agent: the Video tab ---------------------------------------------
# vidtab_test: the controller (core/debugger/vidtab.c) on made-up captures plus a
# fuzz; no machine, no debugger, so it needs no BIOS and no SDL. The
# COLECO_DEBUGGER_VIDEO_* variables are read when a tab is created: the second
# test sets them (no setenv in the test: MinGW-safe).
add_executable(vidtab_test vidtab_test.c)
target_link_libraries(vidtab_test coleco_core)
add_test(NAME vidtab COMMAND vidtab_test)
set_tests_properties(vidtab PROPERTIES TIMEOUT 120)
add_test(NAME vidtab_env COMMAND vidtab_test env)
set_tests_properties(vidtab_env PROPERTIES TIMEOUT 30
  ENVIRONMENT "COLECO_DEBUGGER_VIDEO_VIEW=beam;COLECO_DEBUGGER_VIDEO_SOURCE=palette;COLECO_DEBUGGER_VIDEO_OVERLAYS=grid,splits;COLECO_DEBUGGER_VIDEO_ZOOM=4;COLECO_DEBUGGER_VIDEO_SECTION=timing")
# ---- end VIDEO agent ---------------------------------------------------------

# The real machine on its real thread. Skips (77) without a BIOS, which is
# every CI and release checkout -- this repository does not ship one.
add_executable(boot_smoke boot_smoke.c)
target_link_libraries(boot_smoke coleco_core)
add_test(NAME boot_smoke COMMAND boot_smoke "${CMAKE_SOURCE_DIR}/tools/roms")
set_tests_properties(boot_smoke PROPERTIES SKIP_RETURN_CODE 77)

# The remembered pad -> port table: pure, no SDL, no session.
add_executable(pad_assign_test pad_assign_test.c)
target_link_libraries(pad_assign_test coleco_core)
target_include_directories(pad_assign_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
add_test(NAME pad_assign COMMAND pad_assign_test)

# The key table, pinned to exact controller words.
# These need the session layer, which needs SDL3. On a build without it
# (a cross-toolchain, or a lean machine) the core's own tests still run.
if(TARGET coleco_session)

add_executable(input_test input_test.c)
target_link_libraries(input_test coleco_session)
add_test(NAME input COMMAND input_test)

# The on-screen controllers' layout, hit test and gesture maths.
add_executable(controls_test controls_test.c)
target_link_libraries(controls_test coleco_session)
add_test(NAME controls COMMAND controls_test)

# The frontend contract, against a real temporary XDG tree.
add_executable(session_test session_test.c)
target_link_libraries(session_test coleco_session)
add_test(NAME session COMMAND session_test "${CMAKE_SOURCE_DIR}/tools/roms")

# The remappable binding table, the key translator and the 0.9.0 migration.
add_executable(bindings_test bindings_test.c)
target_link_libraries(bindings_test coleco_session)
add_test(NAME bindings COMMAND bindings_test)

# The built-in Help covers every setting, key and context.
add_executable(help_test help_test.c)
target_link_libraries(help_test PRIVATE coleco_session)
add_test(NAME help COMMAND help_test)

# The external-FujiNet address field's parser.
add_executable(hostport_test hostport_test.c)
target_link_libraries(hostport_test coleco_session)
add_test(NAME hostport COMMAND hostport_test)

# External FujiNet: the cartridge dials a stand-in listener and no built-in
# runtime is started. Needs a BIOS (SKIPs, 77, without); not on Windows.
add_executable(external_test external_test.c)
target_link_libraries(external_test coleco_session)
add_test(NAME external COMMAND external_test "${CMAKE_SOURCE_DIR}/tools/roms")
set_tests_properties(external PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# Import BIOS end to end, without a machine: a file, a zip, a folder 8 levels
# deep with zips in it, the unknown-CRC rule, and a dropped BIOS routed to the
# ROM directory. Generated fillers with the right CRC (no firmware).
add_executable(roms_import_test roms_import_test.c)
target_link_libraries(roms_import_test coleco_session)
target_include_directories(roms_import_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
add_test(NAME roms_import COMMAND roms_import_test)

# Where a dropped file goes, and whether a real cartridge runs once there.
add_executable(media_test media_test.c)
target_link_libraries(media_test coleco_session)
add_test(NAME media COMMAND media_test "${CMAKE_SOURCE_DIR}/tools/roms")

# The debugger engine against a real running machine. Skips without a BIOS.
# Neither of these needs a ROM: dasm_test drives the disassembler on
# hand-written byte sequences, and vdp_decode_test builds VDP snapshots in
# memory. So unlike debug_test they always run rather than SKIP.
add_executable(dasm_test dasm_test.c)
target_link_libraries(dasm_test coleco_session)
target_include_directories(dasm_test PRIVATE "${CMAKE_SOURCE_DIR}/core/debugger")
add_test(NAME dasm COMMAND dasm_test)

add_executable(vdp_decode_test vdp_decode_test.c)
target_link_libraries(vdp_decode_test coleco_session)
add_test(NAME vdp_decode COMMAND vdp_decode_test)

add_executable(debug_test debug_test.c)
target_link_libraries(debug_test coleco_session)
add_test(NAME debug COMMAND debug_test "${CMAKE_SOURCE_DIR}/tools/roms")
set_tests_properties(debug PROPERTIES SKIP_RETURN_CODE 77)


# ---- VIDEO agent: the Video tab on a real machine ---------------------------
# vidtab_live_test: Display == the published frame at a stop, Beam, a step, a
# VRAM poke, the console text. Needs a BIOS and SKIPs (77) without one.
add_executable(vidtab_live_test vidtab_live_test.c)
target_link_libraries(vidtab_live_test coleco_session)
add_test(NAME vidtab_live COMMAND vidtab_live_test "${CMAKE_SOURCE_DIR}/tools/roms")
set_tests_properties(vidtab_live PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
# ---- end VIDEO agent ---------------------------------------------------------

# The gamepad layer's pure helpers, and its start and stop with no hardware.
add_executable(gamepad_test gamepad_test.c)
target_link_libraries(gamepad_test coleco_session)
target_include_directories(gamepad_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
add_test(NAME gamepad COMMAND gamepad_test)
set_tests_properties(gamepad PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# The gamepad layer end to end on SDL3 virtual joysticks (a gamepad and a raw
# adapter), with no machine: runs in CI. SKIPs (77) on a runner with no
# virtual joystick support. The waits poll with a ceiling, and a slow runner
# is slow, so the timeout is generous.
add_executable(gamepad_map_test gamepad_map_test.c)
target_link_libraries(gamepad_map_test coleco_session)
target_include_directories(gamepad_map_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
add_test(NAME gamepad_map COMMAND gamepad_map_test)
set_tests_properties(gamepad_map PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# What a power cycle (Reset to CONFIG) keeps: the held keys and the pads.
# Needs a real BIOS and SKIPs (77) without one, as on every CI checkout.
add_executable(power_cycle_test power_cycle_test.c)
target_link_libraries(power_cycle_test coleco_session)
add_test(NAME power_cycle COMMAND power_cycle_test "${CMAKE_SOURCE_DIR}/tools/roms")
set_tests_properties(power_cycle PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# Interactive: needs a pad and a human. Built, never added as a ctest -- no CI
# runner can press a button, and nothing automated can tell you whether a
# given controller reports its d-pad as a hat, as buttons or as axes.
add_executable(gamepad_probe gamepad_probe.c)
target_link_libraries(gamepad_probe coleco_session)

endif() # TARGET coleco_session

# On Windows the tests that start threads otherwise need libwinpthread-1.dll
# beside them, which only happens to be true inside an MSYS2 shell. Link the
# runtime statically (as the frontend does) so the test exes run anywhere --
# on a bare Windows box, or under wine when cross-compiling.
if(WIN32)
  # cart_test is conditional on the ROMs being available, hence the guard.
  foreach(_t no_sdl_link_test cart_test fujitrace_test boot_smoke input_test
             session_test bindings_test roms_import_test media_test hostport_test external_test debug_test dasm_test
             vdp_decode_test pad_assign_test controls_test gamepad_test gamepad_map_test power_cycle_test gamepad_probe)
    if(TARGET ${_t})
      target_link_options(${_t} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
  endforeach()
endif()

# ---- VIDEO agent: static runtime for the Windows test exes -------------------
if(WIN32)
  foreach(_t vidtab_test vidtab_live_test)
    if(TARGET ${_t})
      target_link_options(${_t} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
  endforeach()
endif()
# ---- end VIDEO agent ---------------------------------------------------------

# ---- ENGINE agent: static runtime for the Windows test exes ------------------
if(WIN32)
  foreach(_t expr_test memtools_test prompt_test engine_test)
    if(TARGET ${_t})
      target_link_options(${_t} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
  endforeach()
endif()
# ---- end ENGINE agent --------------------------------------------------------

# The rate-controlled audio ring (core/coleco/audioring.h), header-only: ten
# simulated minutes at +-0.13% clock error. No machine, no SDL.
add_executable(audioring_test audioring_test.c)
target_include_directories(audioring_test PRIVATE "${CMAKE_SOURCE_DIR}/core/coleco")
add_test(NAME audioring COMMAND audioring_test)
