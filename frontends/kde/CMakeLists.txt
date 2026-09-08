# The KDE frontend: plain Qt6 Widgets.
#
# Deliberately NOT KDE Frameworks. It picks up Breeze through the platform
# theme anyway, and staying framework-free keeps this frontend usable outside
# a KDE session -- and keeps the door open to reusing it elsewhere.
#
# Found-or-skipped, like the GNOME one: a machine without Qt6 development
# packages still builds the core, the tests and the other frontend.

# Scoped to this directory: the project is C, and this frontend is the only
# C++ in it. It has to come BEFORE find_package(Qt6), not after -- Qt's
# config runs check_cxx_source_compiles (for WrapAtomic among others), which
# fails outright if no C++ compiler has been enabled yet.
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 COMPONENTS Widgets QUIET)
if(NOT Qt6_FOUND)
  message(STATUS "kde frontend: Qt6 Widgets not found -- skipping")
  return()
endif()

add_executable(fujinet-go-coleco-kde
  main.cpp
  MainWindow.cpp
  DisplayWidget.cpp
  KeypadWindow.cpp
  KeyForward.cpp
  SettingsDialog.cpp
  debugger/DebuggerWindow.cpp
)
target_link_libraries(fujinet-go-coleco-kde PRIVATE coleco_session Qt6::Widgets)
target_compile_options(fujinet-go-coleco-kde PRIVATE -Wall -Wextra)

install(TARGETS fujinet-go-coleco-kde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

configure_file(data/online.fujinet.go.coleco.kde.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.coleco.kde.desktop
               @ONLY)
configure_file(data/online.fujinet.go.coleco.kde.metainfo.xml.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.coleco.kde.metainfo.xml
               @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.coleco.kde.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.coleco.kde.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_file_kde
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.coleco.kde.desktop)
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_kde
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.coleco.kde.metainfo.xml)
endif()
