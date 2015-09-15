
# Include the macros and functions.
include(${CMAKE_CURRENT_LIST_DIR}/CMake/CMakeMacros.cmake)

# Set up helper variables for future configuring.
set(SM_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}/CMake")
set(SM_EXTERN_DIR "${CMAKE_CURRENT_LIST_DIR}/extern")
set(SM_BUNDLE_DIR "${CMAKE_CURRENT_LIST_DIR}/bundle")
set(SM_XCODE_DIR "${CMAKE_CURRENT_LIST_DIR}/Xcode")
set(SM_PROGRAM_DIR "${CMAKE_CURRENT_LIST_DIR}/Program")
set(SM_BUILD_DIR "${CMAKE_CURRENT_LIST_DIR}/Build")
set(SM_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/src")
set(SM_DOC_DIR "${CMAKE_CURRENT_LIST_DIR}/Docs")
set(SM_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}")

# TODO: Reconsile the OS dependent naming scheme.
set(SM_EXE_NAME "StepMania")

# Some OS specific helpers.
if (CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(LINUX TRUE)
else()
  set(LINUX FALSE)
endif()

if (CMAKE_SYSTEM_NAME MATCHES "Darwin")
  set(MACOSX TRUE)
else()
  set(MACOSX FALSE)
endif()

if (CMAKE_SYSTEM_NAME MATCHES "BSD")
  set(BSD TRUE)
else()
  set(BSD FALSE)
endif()

# Allow for finding our libraries in a standard location.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/CMake/Modules/")

include("${CMAKE_CURRENT_LIST_DIR}/CMake/DefineOptions.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/CMake/SMDefs.cmake")

# Put the predefined targets in separate groups.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Checks the standard include directories for c-style headers.
# We may use C++ in this project, but the check works better with plain C headers.
include(CheckIncludeFiles)
check_include_files(alloca.h HAVE_ALLOCA_H)
check_include_files(assert.h HAVE_ASSERT_H)
check_include_files(dlfcn.h HAVE_DLFCN_H)
check_include_files(dirent.h HAVE_DIRENT_H)
check_include_files(errno.h HAVE_ERRNO_H)
check_include_files(fcntl.h HAVE_FCNTL_H)
check_include_files(float.h HAVE_FLOAT_H)
check_include_files(inttypes.h HAVE_INTTYPES_H)
check_include_files(limits.h HAVE_LIMITS_H)
check_include_files(math.h HAVE_MATH_H)
check_include_files(memory.h HAVE_MEMORY_H)
check_include_files(stdarg.h HAVE_STDARG_H)
check_include_files(stddef.h HAVE_STDDEF_H)
check_include_files(stdint.h HAVE_STDINT_H)
check_include_files(stdlib.h HAVE_STDLIB_H)
check_include_files(strings.h HAVE_STRINGS_H)
check_include_files(string.h HAVE_STRING_H)
check_include_files(unistd.h HAVE_UNISTD_H)
check_include_files(sys/param.h HAVE_SYS_PARAM_H)
check_include_files(sys/stat.h HAVE_SYS_STAT_H)
check_include_files(sys/types.h HAVE_SYS_TYPES_H)
check_include_files(sys/utsname.h HAVE_SYS_UTSNAME_H)
check_include_files(sys/wait.h HAVE_SYS_WAIT_H)

check_include_files(endian.h HAVE_ENDIAN_H)
check_include_files(sys/endian.h HAVE_SYS_ENDIAN_H)
check_include_files(machine/endian.h HAVE_MACHINE_ENDIAN_H)

if (HAVE_STDLIB_H AND HAVE_STDARG_H AND HAVE_STRING_H AND HAVE_FLOAT_H)
  set(STDC_HEADERS 1)
endif()

include(CheckFunctionExists)
include(CheckSymbolExists)
include(CheckCXXSymbolExists)

# Mostly Windows functions.
check_function_exists(_mkdir HAVE__MKDIR)
check_cxx_symbol_exists(_snprintf cstdio HAVE__SNPRINTF)
#check_function_exists(_snprintf HAVE__SNPRINTF)

# Mostly non-Windows functions.
check_function_exists(fcntl HAVE_FCNTL)
check_function_exists(fork HAVE_FORK)
check_function_exists(mkdir HAVE_MKDIR)
#check_function_exists(snprintf HAVE_SNPRINTF)
check_cxx_symbol_exists(snprintf cstdio HAVE_SNPRINTF)
check_function_exists(waitpid HAVE_WAITPID)


# Mostly universal symbols.
check_cxx_symbol_exists(powf cmath HAVE_POWF)
check_cxx_symbol_exists(sqrtf cmath HAVE_SQRTF)
check_cxx_symbol_exists(sinf cmath HAVE_SINF)
check_cxx_symbol_exists(tanf cmath HAVE_TANF)
check_cxx_symbol_exists(cosf cmath HAVE_COSF)
check_cxx_symbol_exists(acosf cmath HAVE_ACOSF)
check_cxx_symbol_exists(truncf cmath HAVE_TRUNCF)
check_cxx_symbol_exists(roundf cmath HAVE_ROUNDF)
check_cxx_symbol_exists(lrintf cmath HAVE_LRINTF)
check_cxx_symbol_exists(strtof cstdlib HAVE_STRTOF)
check_symbol_exists(M_PI math.h HAVE_M_PI)

# Checks to make it easier to work with 32-bit/64-bit builds if required.
include(CheckTypeSize)
check_type_size(short SIZEOF_SHORT)
check_type_size(int SIZEOF_INT)
check_type_size(long SIZEOF_LONG)
check_type_size("long long" SIZEOF_LONG_LONG)
check_type_size(float SIZEOF_FLOAT)
check_type_size(double SIZEOF_DOUBLE)
check_type_size(intptr_t SIZEOF_INTPTR_T)
check_type_size(pid_t SIZEOF_PID_T)
check_type_size(size_t SIZEOF_SIZE_T)
check_type_size(ssize_t SIZEOF_SSIZE_T)

include(TestBigEndian)
test_big_endian(BIGENDIAN)
if (${BIGENDIAN})
  set(ENDIAN_BIG 1)
else()
  set(ENDIAN_LITTLE 1)
endif()

if (WITH_VERSION_INFO)
  set(HAVE_VERSION_INFO 1)
endif()

# Dependencies go here.
include(ExternalProject)

if(NOT WITH_GPL_LIBS)
  message("Disabling GPL exclusive libraries: no MP3 support.")
  set(WITH_MP3 OFF)
endif()

if(WITH_WAV)
  # TODO: Identify which headers to check for ensuring this will always work.
  set(HAS_WAV TRUE)
endif()

if(WITH_MP3)
  if(WIN32 OR MACOSX)
    set(HAS_MP3 TRUE)
  else()
    find_package(Mad)
    if(NOT LIBMAD_FOUND)
      message(FATAL_ERROR "Libmad library not found. If you wish to skip mp3 support, set WITH_MP3 to OFF when configuring.")
    else()
      set(HAS_MP3 TRUE)
    endif()
  endif()
endif()

if(WITH_OGG)
  if(WIN32 OR MACOSX)
    set(HAS_OGG TRUE)
  else()
    find_package(Ogg)
    find_package(Vorbis)
    find_package(VorbisFile)

    if(NOT (OGG_FOUND AND VORBIS_FOUND AND VORBISFILE_FOUND) )
      message(FATAL_ERROR "Not all vorbis libraries were found. If you wish to skip vorbis support, set WITH_OGG to OFF when configuring.")
    else()
      set(HAS_OGG TRUE)
    endif()
  endif()
endif()

if(WIN32)
  set(SYSTEM_PCRE_FOUND FALSE)
  find_package(DirectX REQUIRED)

  # FFMPEG...it can be evil.
  find_library(LIB_SWSCALE NAMES "swscale"
    PATHS "${SM_EXTERN_DIR}/ffmpeg/lib" NO_DEFAULT_PATH
  )
  get_filename_component(LIB_SWSCALE ${LIB_SWSCALE} NAME)

  find_library(LIB_AVCODEC NAMES "avcodec"
    PATHS "${SM_EXTERN_DIR}/ffmpeg/lib" NO_DEFAULT_PATH
  )
  get_filename_component(LIB_AVCODEC ${LIB_AVCODEC} NAME)

  find_library(LIB_AVFORMAT NAMES "avformat"
    PATHS "${SM_EXTERN_DIR}/ffmpeg/lib" NO_DEFAULT_PATH
  )
  get_filename_component(LIB_AVFORMAT ${LIB_AVFORMAT} NAME)

  find_library(LIB_AVUTIL NAMES "avutil"
    PATHS "${SM_EXTERN_DIR}/ffmpeg/lib" NO_DEFAULT_PATH
  )
  get_filename_component(LIB_AVUTIL ${LIB_AVUTIL} NAME)
elseif(MACOSX)
  set(SYSTEM_PCRE_FOUND FALSE)
  set(WITH_CRASH_HANDLER TRUE)
  # Apple Archs needs to be 32-bit for now.
  # When SDL2 is introduced, this may change.
  set(CMAKE_OSX_ARCHITECTURES "i386")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.6")
  set(CMAKE_OSX_DEPLOYMENT_TARGET_FULL "10.6.8")

  find_library(MAC_FRAME_ACCELERATE Accelerate ${CMAKE_SYSTEM_FRAMEWORK_PATH})
  find_library(MAC_FRAME_APPKIT AppKit ${CMAKE_SYSTEM_FRAMEWORK_PATH})
  find_library(MAC_FRAME_AUDIOTOOLBOX AudioToolbox ${CMAKE_SYSTEM_FRAMEWORK_PATH})
  find_library(MAC_FRAME_AUDIOUNIT AudioUnit ${CMAKE_SYSTEM_FRAMEWORK_PATH})
  find_library(MAC_FRAME_CARBON Carbon ${CMAKE_SYSTEM_FRAMEWORK_PATH})
  find_library(MAC_FRAME_COCOA Cocoa ${CMAKE_SYSTEM_FRAMEWORK_PATH})
  find_library(MAC_FRAME_COREAUDIO CoreAudio ${CMAKE_SYSTEM_FRAMEWORK_PATH})
  find_library(MAC_FRAME_COREFOUNDATION CoreFoundation ${CMAKE_SYSTEM_FRAMEWORK_PATH})
  find_library(MAC_FRAME_CORESERVICES CoreServices ${CMAKE_SYSTEM_FRAMEWORK_PATH})
  find_library(MAC_FRAME_FOUNDATION Foundation ${CMAKE_SYSTEM_FRAMEWORK_PATH})
  find_library(MAC_FRAME_IOKIT IOKit ${CMAKE_SYSTEM_FRAMEWORK_PATH})
  find_library(MAC_FRAME_OPENGL OpenGL ${CMAKE_SYSTEM_FRAMEWORK_PATH})
  find_library(MAC_FRAME_QUICKTIME QuickTime ${CMAKE_SYSTEM_FRAMEWORK_PATH})

  mark_as_advanced(
    MAC_FRAME_ACCELERATE
    MAC_FRAME_APPKIT
    MAC_FRAME_AUDIOTOOLBOX
    MAC_FRAME_AUDIOUNIT
    MAC_FRAME_CARBON
    MAC_FRAME_COCOA
    MAC_FRAME_COREAUDIO
    MAC_FRAME_COREFOUNDATION
    MAC_FRAME_CORESERVICES
    MAC_FRAME_FOUNDATION
    MAC_FRAME_IOKIT
    MAC_FRAME_OPENGL
    MAC_FRAME_QUICKTIME
  )
elseif(LINUX)
  if(WITH_GTK2)
    find_package("GTK2" 2.0)
    if (${GTK2_FOUND})
      set(HAS_GTK2 TRUE)
    else()
      set(HAS_GTK2 FALSE)
      message("GTK2 was not found on your system. There will be no loading window.")
    endif()
  else()
    set(HAS_GTK2 FALSE)
  endif()

  find_package(X11)
  if(${X11_FOUND})
    set(HAS_X11 TRUE)
  else()
    set(HAS_X11 FALSE)
  endif()

  find_package(Pcre)
  set(SYSTEM_PCRE_FOUND ${PCRE_FOUND})

  find_package("BZip2")
  if (NOT(${BZIP2_FOUND}))
    message(FATAL_ERROR "Bzip2 support required.")
  endif()

  find_package("ZLIB")
  if (NOT(${ZLIB_FOUND}))
    message(FATAL_ERROR "zlib support required.")
  endif()

  find_package("JPEG")
  if(NOT(${JPEG_FOUND}))
    message(FATAL_ERROR "jpeg support required.")
  endif()

  find_library(DL_LIBRARY dl)
  if(${LIBDL_FOUND})
    set(HAS_LIBDL TRUE)
  else()
    set(HAS_LIBDL FALSE)
  endif()

  find_package(Xrandr)
  if (${XRANDR_FOUND})
    set(HAS_XRANDR TRUE)
  else()
    set(HAX_XRANDR FALSE)
  endif()

  find_package(PulseAudio)
  if (PULSEAUDIO_FOUND)
    set(HAS_PULSE TRUE)
  else()
    set(HAS_PULSE FALSE)
  endif()

  find_package(ALSA)
  if (ALSA_FOUND)
    set(HAS_ALSA TRUE)
  else()
    set(HAS_ALSA FALSE)
  endif()

  find_package(JACK)
  if (JACK_FOUND)
    set(HAS_JACK TRUE)
  else()
    set(HAS_JACK FALSE)
  endif()

  find_package(OSS)
  if (OSS_FOUND)
    set(HAS_OSS TRUE)
  else()
    set(HAS_OSS FALSE)
  endif()

  if(NOT OSS_FOUND AND NOT JACK_FOUND AND NOT ALSA_FOUND AND NOT PULSE_FOUND)
    message(FATAL_ERROR "No sound libraries found. You will require at least one.")
  else()
    message(STATUS "-- At least one sound library was found. Do not worry if any were not found at this stage.")
  endif()

  find_package(Threads)
  if (${Threads_FOUND})
    set(HAS_PTHREAD TRUE)
  else()
    set(HAS_PTHREAD FALSE)
  endif()

  find_package(yasm)
  if (NOT YASM_FOUND)
    message("YASM was not found. Please install if you wish for ffmpeg support.")
    set(WITH_FFMPEG OFF)
  endif()

  find_package("Va")

  if(WITH_FFMPEG)
    if (WITH_SYSTEM_FFMPEG)
      find_package("FFMPEG")
      if(NOT FFMPEG_FOUND)
        message(FATAL_ERROR "System ffmpeg not found! Either install the libraries or remove the argument, then try again.")
      else()

        message(STATUS "-- Warning! Your version of ffmpeg may be too high! If you want to use the system ffmpeg, clear your cmake cache and do not include the system ffmpeg argument.")
        set(HAS_FFMPEG TRUE)
      endif()
    else()
      set(SM_FFMPEG_VERSION "2.1.3")
      set(SM_FFMPEG_SRC_LIST "${SM_EXTERN_DIR}" "/ffmpeg-linux-" "${SM_FFMPEG_VERSION}")
      sm_join("${SM_FFMPEG_SRC_LIST}" "" SM_FFMPEG_SRC_DIR)
      set(SM_FFMPEG_ROOT "${CMAKE_BINARY_DIR}/ffmpeg-prefix/src/ffmpeg-build")
      list(APPEND FFMPEG_CONFIGURE
        "${SM_FFMPEG_SRC_DIR}/configure"
        "--disable-programs"
        "--disable-doc"
        "--disable-avdevice"
        "--disable-swresample"
        "--disable-postproc"
        "--disable-avfilter"
        "--disable-shared"
        "--enable-static"
      )
      if(WITH_GPL_LIBS)
        list(APPEND FFMPEG_CONFIGURE
          "--enable-gpl"
        )
      endif()

      if (WITH_CRYSTALHD_DISABLED)
        list(APPEND FFMPEG_CONFIGURE "--disable-crystalhd")
      endif()

      if (NOT WITH_EXTERNAL_WARNINGS)
        list(APPEND FFMPEG_CONFIGURE
          "--extra-cflags=-w"
        )
      endif()

      if (IS_DIRECTORY "${SM_FFMPEG_SRC_DIR}")
        externalproject_add("ffmpeg"
          SOURCE_DIR "${SM_FFMPEG_SRC_DIR}"
          CONFIGURE_COMMAND ${FFMPEG_CONFIGURE}
          BUILD_COMMAND "make"
          UPDATE_COMMAND ""
          INSTALL_COMMAND ""
          TEST_COMMAND ""
        )
      else()
        # --shlibdir=$our_installdir/stepmania-$VERSION
        externalproject_add("ffmpeg"
          DOWNLOAD_COMMAND git clone "--branch" "n${SM_FFMPEG_VERSION}" "--depth" "1" "git://source.ffmpeg.org/ffmpeg.git" "${SM_FFMPEG_SRC_DIR}"
          CONFIGURE_COMMAND "${FFMPEG_CONFIGURE}"
          BUILD_COMMAND "make"
          UPDATE_COMMAND ""
          INSTALL_COMMAND ""
          TEST_COMMAND ""
        )
      endif()
      set(HAS_FFMPEG TRUE)
    endif()
  else()
    set(HAS_FFMPEG FALSE)
  endif()

  find_package(OpenGL REQUIRED)
  if (NOT ${OPENGL_FOUND})
    message(FATAL_ERROR "OpenGL required to compile StepMania.")
  endif()

  find_package(GLEW REQUIRED)
  if (NOT ${GLEW_FOUND})
    message(FATAL_ERROR "GLEW required to compile StepMania.")
  endif()

endif()

configure_file("${SM_SRC_DIR}/config.in.hpp" "${SM_SRC_DIR}/generated/config.hpp")

# Define installer based items for cpack.
include("${CMAKE_CURRENT_LIST_DIR}/CMake/CPackSetup.cmake")