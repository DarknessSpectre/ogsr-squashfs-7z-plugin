cmake_minimum_required(VERSION 3.16)
project(sq_plugin LANGUAGES C CXX)

set(ROOT ${CMAKE_CURRENT_LIST_DIR})
set(Z7_ROOT ${ROOT}/third_party/7zip)
set(LZ4_ROOT ${ROOT}/third_party/lz4)

if (NOT EXISTS "${Z7_ROOT}/CPP/7zip/Archive/IArchive.h")
  message(FATAL_ERROR "third_party/7zip is missing: unpack the 7-Zip sources there")
endif()

set(SRC_SQ
  ${ROOT}/CPP/7zip/Archive/Sq/SqIn.cpp
  ${ROOT}/CPP/7zip/Archive/Sq/SqOut.cpp
  ${ROOT}/CPP/7zip/Archive/Sq/SqRegister.cpp
  ${LZ4_ROOT}/lz4.c
  ${LZ4_ROOT}/lz4hc.c
  ${Z7_ROOT}/CPP/7zip/Archive/ArchiveExports.cpp
  ${Z7_ROOT}/CPP/7zip/Archive/DllExports2.cpp
)

set(SRC_7Z
  ${Z7_ROOT}/CPP/7zip/Common/CreateCoder.cpp
  ${Z7_ROOT}/CPP/7zip/Common/FilterCoder.cpp
  ${Z7_ROOT}/CPP/7zip/Common/PropId.cpp
  ${Z7_ROOT}/CPP/Common/Wildcard.cpp
  ${Z7_ROOT}/CPP/7zip/Common/MethodId.cpp
  ${Z7_ROOT}/CPP/7zip/Common/MethodProps.cpp
  ${Z7_ROOT}/CPP/7zip/Common/StreamUtils.cpp
  ${Z7_ROOT}/CPP/7zip/Compress/CodecExports.cpp
  ${Z7_ROOT}/CPP/Common/IntToString.cpp
  ${Z7_ROOT}/CPP/Common/MyString.cpp
  ${Z7_ROOT}/CPP/Common/MyVector.cpp
  ${Z7_ROOT}/CPP/Common/NewHandler.cpp
  ${Z7_ROOT}/CPP/Common/StringConvert.cpp
  ${Z7_ROOT}/CPP/Common/StringToInt.cpp
  ${Z7_ROOT}/CPP/Common/UTFConvert.cpp
  ${Z7_ROOT}/CPP/Windows/PropVariant.cpp
  ${Z7_ROOT}/CPP/Windows/System.cpp
  ${Z7_ROOT}/CPP/Windows/TimeUtils.cpp
  ${Z7_ROOT}/C/Alloc.c
  ${Z7_ROOT}/C/CpuArch.c
  ${Z7_ROOT}/C/Sha256.c
  ${Z7_ROOT}/C/Sha256Opt.c
)

add_library(Sq SHARED ${SRC_SQ} ${SRC_7Z})

target_include_directories(Sq PRIVATE
  ${ROOT}/CPP/7zip/Archive/Sq
  ${LZ4_ROOT}
  ${Z7_ROOT}/CPP
  ${Z7_ROOT}/CPP/7zip
  ${Z7_ROOT}/C
)

target_compile_definitions(Sq PRIVATE
  _WIN32_WINNT=0x0601
  UNICODE
  _UNICODE
  _CRT_SECURE_NO_WARNINGS
  LZ4HC_HEAPMODE=0
)

if(MSVC)
  target_compile_options(Sq PRIVATE /W3 /EHsc /O2)
  # Static CRT: the DLL does not depend on the Visual C++ runtime being installed.
  set_property(TARGET Sq PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(Sq PROPERTIES
  OUTPUT_NAME "SQ_$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,x64,x86>"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_link_options(Sq PRIVATE "/DEF:${Z7_ROOT}/CPP/7zip/Archive/Archive2.def")
