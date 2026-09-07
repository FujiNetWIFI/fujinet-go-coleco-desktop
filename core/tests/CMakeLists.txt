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
  add_test(NAME cart COMMAND cart_test)
endif()

# The real machine on its real thread. Skips (77) without a BIOS, which is
# every CI and release checkout -- this repository does not ship one.
add_executable(boot_smoke boot_smoke.c)
target_link_libraries(boot_smoke coleco_core)
add_test(NAME boot_smoke COMMAND boot_smoke "${CMAKE_SOURCE_DIR}/tools/roms")
set_tests_properties(boot_smoke PROPERTIES SKIP_RETURN_CODE 77)

# The key table, pinned to exact controller words.
# These need the session layer, which needs SDL3. On a build without it
# (a cross-toolchain, or a lean machine) the core's own tests still run.
if(TARGET coleco_session)

add_executable(input_test input_test.c)
target_link_libraries(input_test coleco_session)
add_test(NAME input COMMAND input_test)

# The frontend contract, against a real temporary XDG tree.
add_executable(session_test session_test.c)
target_link_libraries(session_test coleco_session)
add_test(NAME session COMMAND session_test "${CMAKE_SOURCE_DIR}/tools/roms")

# The remappable binding table and the panel's direct press path.
add_executable(bindings_test bindings_test.c)
target_link_libraries(bindings_test coleco_session)
add_test(NAME bindings COMMAND bindings_test)

# Where a dropped file goes, and whether a real cartridge runs once there.
add_executable(media_test media_test.c)
target_link_libraries(media_test coleco_session)
add_test(NAME media COMMAND media_test "${CMAKE_SOURCE_DIR}/tools/roms")

# The debugger engine against a real running machine. Skips without a BIOS.
add_executable(debug_test debug_test.c)
target_link_libraries(debug_test coleco_session)
add_test(NAME debug COMMAND debug_test "${CMAKE_SOURCE_DIR}/tools/roms")
set_tests_properties(debug PROPERTIES SKIP_RETURN_CODE 77)


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
  foreach(_t no_sdl_link_test cart_test boot_smoke input_test session_test
             bindings_test media_test debug_test gamepad_probe)
    if(TARGET ${_t})
      target_link_options(${_t} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
  endforeach()
endif()
