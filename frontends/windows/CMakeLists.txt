# The Windows frontend: Win32 + GDI, no toolkit.
#
# WIN32_EXECUTABLE gives WinMain and no console window. The manifest is
# generated from PROJECT_VERSION rather than repeating it, so the version
# lives in exactly one place (the top-level project() call).

if(NOT WIN32)
  message(STATUS "windows frontend: not a Windows build -- skipping")
  return()
endif()

configure_file(app.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/app.manifest @ONLY)

# windres needs to find app.ico and app.manifest; the .rc references both by
# bare name, so give it both the source dir (icon) and the binary dir
# (generated manifest).
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_BINARY_DIR}")
enable_language(RC)

add_executable(fujinet-go-coleco-windows WIN32
  main.c
  keypad/keypad_window.c
  resource.rc
)
target_include_directories(fujinet-go-coleco-windows PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fujinet-go-coleco-windows PRIVATE
  coleco_session
  # dwmapi: DwmFlush, this platform's frame clock. comdlg32: the file
  # dialogs. shell32: DragQueryFile and ShellExecute for the web UI.
  dwmapi comdlg32 shell32)
target_compile_options(fujinet-go-coleco-windows PRIVATE -Wall -Wextra)

install(TARGETS fujinet-go-coleco-windows RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
