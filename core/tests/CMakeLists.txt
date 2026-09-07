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
