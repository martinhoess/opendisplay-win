cmake_minimum_required(VERSION 3.20)
project(opendisplay-win CXX)

if(NOT WIN32)
  message(FATAL_ERROR "opendisplay-win only builds on Windows (Media Foundation, DXGI, Win32).")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT so a released .exe runs on a bare Windows box without the VC++
# Redistributable installed. Costs ~250 KB; the UCRT part (api-ms-win-crt-*)
# ships with Windows 10/11 either way.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(opendisplay-win
  src/main.cpp
  src/net/Connection.cpp
  src/net/UsbMux.cpp
  src/net/Protocol.cpp
  src/net/CursorMessages.cpp
  src/net/CursorUdp.cpp
  src/net/Mdns.cpp
  src/encode/AnnexB.cpp
  src/encode/H264Encoder.cpp
  src/display/DesktopDuplication.cpp
  src/display/ExistingMonitor.cpp
  src/display/MttVddSettings.cpp
  src/input/InputInjector.cpp
  src/app/Log.cpp
  src/app/SenderApp.cpp
  src/app/Config.cpp
  src/app/TrayApp.cpp
  src/app/resources.rc
)

target_include_directories(opendisplay-win PRIVATE
  src
)

target_link_libraries(opendisplay-win PRIVATE
  ws2_32
  user32
  d3d11
  dxgi
  dxguid
  mfplat
  mf
  mfreadwrite
  mfuuid
  setupapi
  cfgmgr32
  shell32
  comctl32
  gdi32
  windowscodecs
  ole32
)

target_compile_definitions(opendisplay-win PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

# asInvoker (not requireAdministrator): the app runs un-elevated. The only
# operations that need admin are writes to the MTT driver's settings dir
# (C:\VirtualDisplayDriver: user_edid.bin, vdd_settings.xml flags / monitor
# count); those self-elevate a one-off (`--ensure-mtt-edid`,
# `--set-mtt-monitor-count`). Running the whole app elevated is optional (tray
# menu) - only needed to inject touch into elevated windows.
#
# GUI subsystem so the tray app doesn't flash a console window, but keep
# mainCRTStartup as the entry point so we still have a normal main(argc, argv)
# (populated from the command line) for the headless CLI mode.
target_link_options(opendisplay-win PRIVATE
  "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
  "/SUBSYSTEM:WINDOWS"
  "/ENTRY:mainCRTStartup"
)


# Standalone Media Foundation H.264 encoder probe (no capture/network).
add_executable(mft_encoder_probe
  tools/mft_encoder_probe/main.cpp
)
target_compile_definitions(mft_encoder_probe PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)
target_link_libraries(mft_encoder_probe PRIVATE
  mfplat
  mf
  mfuuid
  ole32
  d3d11
  dxgi
)
# Console subsystem so printf is visible when run from a terminal.
set_target_properties(mft_encoder_probe PROPERTIES WIN32_EXECUTABLE FALSE)

# Ship MTT CustomEdid binary next to the exe (EnsureCustomEdid looks here first).
add_custom_command(TARGET opendisplay-win POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:opendisplay-win>/assets/mtt"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_SOURCE_DIR}/assets/mtt/user_edid.bin"
          "$<TARGET_FILE_DIR:opendisplay-win>/assets/mtt/user_edid.bin"
  COMMENT "Copy MTT user_edid.bin next to opendisplay-win"
)


