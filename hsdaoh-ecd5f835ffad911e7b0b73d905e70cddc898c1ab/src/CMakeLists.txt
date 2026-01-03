# Copyright 2012-2024 Osmocom Project
#
# This file is part of hsdaoh
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

########################################################################
# Setup shared library variant
########################################################################
add_library(hsdaoh STATIC libhsdaoh.c)
target_link_libraries(hsdaoh ${LIBUSB_LIBRARIES} ${LIBUVC_LIBRARIES} ${THREADS_PTHREADS_LIBRARY})
target_include_directories(hsdaoh PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>  # <prefix>/include
  ${LIBUSB_INCLUDE_DIRS}
  ${LIBUVC_INCLUDE_DIRS}
  ${THREADS_PTHREADS_INCLUDE_DIR}
  )
set_target_properties(hsdaoh PROPERTIES DEFINE_SYMBOL "hsdaoh_EXPORTS")
set_target_properties(hsdaoh PROPERTIES OUTPUT_NAME hsdaoh)
set_target_properties(hsdaoh PROPERTIES SOVERSION ${MAJOR_VERSION})
set_target_properties(hsdaoh PROPERTIES VERSION ${LIBVER})
generate_export_header(hsdaoh)

########################################################################
# Setup static library variant
########################################################################
add_library(hsdaoh_static STATIC libhsdaoh.c)
target_link_libraries(hsdaoh m ${LIBUSB_LIBRARIES} ${LIBUVC_LIBRARIES} ${THREADS_PTHREADS_LIBRARY})
target_include_directories(hsdaoh_static PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>  # <prefix>/include
  ${LIBUSB_INCLUDE_DIRS}
  ${LIBUVC_INCLUDE_DIRS}
  ${THREADS_PTHREADS_INCLUDE_DIR}
  )
set_property(TARGET hsdaoh_static APPEND PROPERTY COMPILE_DEFINITIONS "hsdaoh_STATIC" )
if(NOT WIN32)
# Force same library filename for static and shared variants of the library
set_target_properties(hsdaoh_static PROPERTIES OUTPUT_NAME hsdaoh)
endif()
generate_export_header(hsdaoh_static)

########################################################################
# Set up Windows DLL resource files
########################################################################
IF(MSVC)
    include(${CMAKE_SOURCE_DIR}/cmake/Modules/Version.cmake)

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/hsdaoh.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/hsdaoh.rc
    @ONLY)
  target_sources(hsdaoh PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/hsdaoh.rc)
  target_sources(hsdaoh_static PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/hsdaoh.rc)
ENDIF(MSVC)

########################################################################
# Setup libraries used in 
# Install built library files & utilities
########################################################################
install(TARGETS hsdaoh EXPORT HSDAOH-export
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} # .so/.dylib file
  )
install(TARGETS hsdaoh_static EXPORT HSDAOH-export
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} # .so/.dylib file
  )

