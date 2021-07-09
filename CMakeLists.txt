cmake_minimum_required(VERSION 3.10)
project(inputstream.adaptive)
option(BUILD_TESTING "Build the testing tree." ON)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

find_package(Kodi REQUIRED)

set(ADP_SOURCES
	src/main.cpp
	src/common/AdaptiveTree.cpp
	src/parser/DASHTree.cpp
	src/parser/HLSTree.cpp
	src/parser/SmoothTree.cpp
	src/parser/TTML.cpp
	src/parser/WebVTT.cpp
	src/parser/PRProtectionParser.cpp
	src/common/AdaptiveStream.cpp
	src/helpers.cpp
	src/oscompat.cpp
	src/TSReader.cpp
	src/aes_decrypter.cpp
	src/ADTSReader.cpp
	src/md5.cpp
	src/WebmReader.cpp
	)

set(ADP_HEADERS
	src/helpers.h
	src/main.h
	src/oscompat.h
	src/SSD_dll.h
	src/common/AdaptiveStream.h
	src/common/AdaptiveTree.h
	src/parser/DASHTree.h
	src/parser/HLSTree.h
	src/parser/SmoothTree.h
	src/parser/TTML.h
	src/parser/WebVTT.h
	src/parser/PRProtectionParser.h
	src/TSReader.h
	src/log.h
	src/aes_decrypter.h
	src/ADTSReader.h
	src/Iaes_decrypter.h
	src/md5.h
	src/WebmReader.h
	)

if(WIN32)
  find_package(dlfcn-win32 REQUIRED)
  list(APPEND DEPLIBS ${dlfcn-win32_LIBRARIES})
  include_directories(${dlfcn-win32_INCLUDE_DIRS})
endif()

include_directories(${INCLUDES}
                    ${KODI_INCLUDE_DIR}/.. # Hack way with "/..", need bigger Kodi cmake rework to match right include ways (becomes done in future)
                    lib/libbento4/Core
                    lib/webm_parser/include
)

set(CMAKE_FIND_FRAMEWORK LAST)

include(FindEXPAT)
find_package(EXPAT REQUIRED)
include_directories(${EXPAT_INCLUDE_DIRS})
list(APPEND DEPLIBS ${EXPAT_LIBRARIES})

if(WIN32)
  add_definitions(-DXML_STATIC -D_CRT_NONSTDC_NO_DEPRECATE)
  list(APPEND DEPLIBS ws2_32)
else()
  add_definitions(-D__STDC_FORMAT_MACROS)
endif()

add_definitions(-DUNICODE -D_UNICODE)

add_subdirectory(lib/mpegts)
add_subdirectory(wvdecrypter)
add_subdirectory(lib/webm_parser)

set(ADP_ADDITIONAL_BINARY $<TARGET_FILE:ssd_wv>)
set(DECRYPTERPATH "special://home/cdm")

list(APPEND DEPLIBS bento4)
list(APPEND DEPLIBS mpegts)
list(APPEND DEPLIBS webm_parser)

build_addon(inputstream.adaptive ADP DEPLIBS)

include(CPack)

if(NOT CMAKE_CROSSCOMPILING AND BUILD_TESTING)
  list(APPEND CMAKE_CTEST_ARGUMENTS "--output-on-failure")
  enable_testing()
  include(FindGtest)
  find_package(Gtest REQUIRED)
  include_directories(${GTEST_INCLUDE_DIRS})
  add_subdirectory(src/test)
endif()
