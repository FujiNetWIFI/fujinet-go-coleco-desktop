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
