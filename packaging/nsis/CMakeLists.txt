# CMakeLists.txt
#
# Wireshark - Network traffic analyzer
# By Gerald Combs <gerald@wireshark.org>
# Copyright 1998 Gerald Combs
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

# We should use CPack to generate the NSIS package. Even better,
# we should use CPack to create a .msi using WIX.

set(NSIS_GENERATED_FILES
	${CMAKE_CURRENT_BINARY_DIR}/all-manifest.nsh
	${CMAKE_CURRENT_BINARY_DIR}/config.nsh
	${CMAKE_CURRENT_BINARY_DIR}/gtk-dll-manifest.nsh
	${CMAKE_CURRENT_BINARY_DIR}/qt-dll-manifest.nsh
)
set(NSIS_GENERATED_FILES ${NSIS_GENERATED_FILES} PARENT_SCOPE)

set(NSIS_FILES
	wireshark.nsi
	uninstall.nsi
	common.nsh
	GetWindowsVersion.nsh
	servicelib.nsh
	AdditionalTasksPage.ini
	WinPcapPage.ini
	${NSIS_GENERATED_FILES}
	PARENT_SCOPE
)

# Check for leftover NMake-generated files.
foreach(_nsh_file all-manifest.nsh config.nsh gtk-dll-manifest.nsh qt-dll-manifest.nsh)
	if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_nsh_file})
		message(FATAL_ERROR
			"Found ${CMAKE_CURRENT_SOURCE_DIR}/${_nsh_file}.\n"
			"Please run nmake -f Makefile.nmake distclean in ${CMAKE_SOURCE_DIR}."
		)
	endif()
endforeach()

# Variables required for config.nsh
set(PROGRAM_NAME ${CMAKE_PROJECT_NAME})
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" TOP_SRC_DIR)
# STAGING_DIR depends on the build configuration so we pass it
# on the command line below.
file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/docbook" USER_GUIDE_DIR)
if ("${WIRESHARK_TARGET_PLATFORM}" STREQUAL "win32")
        set(TARGET_MACHINE x86)
elseif ("${WIRESHARK_TARGET_PLATFORM}" STREQUAL "win64")
        set(TARGET_MACHINE x64)
else()
        message(FATAL_ERROR "Your mysterious moon-man architecture \"${WIRESHARK_TARGET_PLATFORM}\" frightens and confuses us.")
endif()

# Path to the WinPcap installer.
# XXX Come up with a better variable, e.g. cache WIRESHARK_LIB_DIR in FindWSWinLibs.
file(TO_NATIVE_PATH "${GLIB2_DLL_DIR}/../.." _wireshark_lib_dir)
set(WIRESHARK_LIB_DIR "${_wireshark_lib_dir}")

# Must match ${WIRESHARK_LIB_DIR}/WinPcap_X_Y_Z.exe
set(WINPCAP_PACKAGE_VERSION 4_1_3)
string(REPLACE "_" "." PCAP_DISPLAY_VERSION "${WINPCAP_PACKAGE_VERSION}")

set(PRODUCT_VERSION ${PROJECT_MAJOR_VERSION}.${PROJECT_MINOR_VERSION}.${PROJECT_PATCH_VERSION}.${PROJECT_BUILD_VERSION})
# XXX Shouldn't this reflect the API / ABI version?
set(WTAP_VERSION ${PROJECT_VERSION})

# To do:
# - Sync the various version names between CMake, NMake, and NSIS.
# - Set CMakeLists.txt version strings in make-version.pl
# - Add a VERSION_EXTRA cmake option
set (VERSION "${PROJECT_VERSION}")
set (PRODUCT_VERSION=${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_MICRO}.${VERSION_BUILD})

#add_custom_target(build_nsis_package
#	DEPENDS
#		wireshark-$(WIRESHARK_TARGET_PLATFORM)-$(VERSION).exe
#)

if(BUILD_wireshark AND QT_FOUND)
	set (QT_DIR "\${STAGING_DIR}")
endif()

if(BUILD_wireshark_gtk AND GTK_FOUND)
	set (GTK_DIR "\${STAGING_DIR}")
endif()

# CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS will likely give us a list of DLL
# paths containing spaces. We'll assume that they're all in the same
# directory and use it to create something that's easier to pass to
# NSIS.
set(MSVCR_DLL)
list(GET CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS 0 _msvcr_dll)
if(_msvcr_dll)
	get_filename_component(_msvcr_dir ${_msvcr_dll} DIRECTORY)
	set(MSVCR_DLL "${_msvcr_dir}/*.*")
	file(TO_NATIVE_PATH "${MSVCR_DLL}" MSVCR_DLL)
endif()

# This *should* be compatible with the way we currently do things.
if(MSVC12)
	set(_vcredist_name "vcredist_${TARGET_MACHINE}.exe")
	find_program(VCREDIST_EXE "${_vcredist_name}"
		HINTS "${WIRESHARK_LIB_DIR}/vcredist_MSVC2013" "${WIRESHARK_LIB_DIR}"
	)
	file(TO_NATIVE_PATH "${VCREDIST_EXE}" VCREDIST_EXE)
endif()

if(NOT ENABLE_STATIC)
	# XXX Replace ENABLE_LIBWIRESHARK with !ENABLE_STATIC everywhere.
	set(ENABLE_LIBWIRESHARK 1)
endif()

# Ideally we would generate this at compile time using a separate cmake
# module, e.g. cmake/modules/configure_nsis_file.cmake. However we would
# have to figure out a clean way to pass in the variables above.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config.nsh.in" _config_nsh_contents)
string(REPLACE "!define" "#cmakedefine" _config_nsh_contents "${_config_nsh_contents}")
string(CONFIGURE "${_config_nsh_contents}" _config_nsh_contents)
string(REPLACE "#define" "!define" _config_nsh_contents "${_config_nsh_contents}")
string(REPLACE "#undef" "!undef" _config_nsh_contents "${_config_nsh_contents}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/config.nsh" "${_config_nsh_contents}")

# all-manifest.nsh. Can be created at configure time.
set(_all_manifest "${CMAKE_CURRENT_BINARY_DIR}/all-manifest.nsh")
set(_all_manifest_contents "# Files required for all sections. Generated by CMake.\n")
foreach(_dll ${GLIB2_DLLS} ${CARES_DLL} ${GCRYPT_DLLS} ${GEOIP_DLL}
		${GNUTLS_DLLS} ${KERBEROS_DLLS} ${LUA_DLL} ${SMI_DLL}
		${WINSPARKLE_DLL} ${ZLIB_DLL}
	)
	set(_all_manifest_contents "${_all_manifest_contents}File \"\${STAGING_DIR}\\${_dll}\"\n")
endforeach()
set(_all_manifest_contents "${_all_manifest_contents}File \"\${STAGING_DIR}\\gspawn-${WIRESHARK_TARGET_PLATFORM}-helper.exe\"\n")
set(_all_manifest_contents "${_all_manifest_contents}File \"\${STAGING_DIR}\\gspawn-${WIRESHARK_TARGET_PLATFORM}-helper-console.exe\"\n")
file(WRITE "${_all_manifest}" "${_all_manifest_contents}")

# gtk-dll-manifest.nsh. Can be created at configure time.
set(_gtk_dll_manifest "${CMAKE_CURRENT_BINARY_DIR}/gtk-dll-manifest.nsh")
set(_gtk_dll_manifest_contents "# Files required for the GTK+ section. Generated by CMake.\n")
if(BUILD_wireshark_gtk AND GTK_FOUND)
	foreach(_dll ${GTK2_DLLS} ${GTK3_DLLS})
		set(_gtk_dll_manifest_contents "${_gtk_dll_manifest_contents}File \"\${STAGING_DIR}\\${_dll}\"\n")
	endforeach()
	set(_gtk_dll_manifest_contents "${_gtk_dll_manifest_contents}# Subdirectories\n")
	if(GTK2_ETC_DIR)
		set(_gtk_dll_manifest_contents "${_gtk_dll_manifest_contents}SetOutPath \$INSTDIR\\etc\\gtk-2.0\n")
		set(_gtk_dll_manifest_contents "${_gtk_dll_manifest_contents}File \"\${STAGING_DIR}\\etc\\gtk-2.0\\*.*\"\n")
	endif()
	if(GTK2_ENGINES_DLL_DIR)
		set(_gtk_dll_manifest_contents "${_gtk_dll_manifest_contents}SetOutPath \$INSTDIR\\lib\\gtk-2.0\\2.10.0\\engines\n")
		set(_gtk_dll_manifest_contents "${_gtk_dll_manifest_contents}File \"\${STAGING_DIR}\\lib\\gtk-2.0\\2.10.0\\engines\\*.*\"\n")
	endif()
	set(_gtk_dll_manifest_contents "${_gtk_dll_manifest_contents}SetOutPath \$INSTDIR\\lib\\gtk-2.0\\modules\n")
	set(_gtk_dll_manifest_contents "${_gtk_dll_manifest_contents}File \"\${STAGING_DIR}\\lib\\gtk-2.0\\modules\\*.*\"\n")
	# XXX Schemas (GTK3)
endif()
file(WRITE "${_gtk_dll_manifest}" "${_gtk_dll_manifest_contents}")

file(TO_NATIVE_PATH "${DATAFILE_DIR}" _staging_dir)
file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}" _nsis_include_dir)

# Variables we can't set via config.nsh.
set(NSIS_DEFINES
	-DSTAGING_DIR=${_staging_dir}
	-DNSIS_INCLUDE_DIR=${_nsis_include_dir}
	PARENT_SCOPE
)

# We want to sign all of the executables that we ship in the official
# installers. This means that uninstall.exe must be built separately AND
# that building the installer itself won't overwrite uninstall.exe
macro( ADD_NSIS_UNINSTALLER_TARGET )
	set (_nsis_source_dir ${CMAKE_SOURCE_DIR}/packaging/nsis )
	set (_nsis_binary_dir ${CMAKE_BINARY_DIR}/packaging/nsis )

	add_custom_target(nsis_uninstaller
		DEPENDS ${DATAFILE_DIR}/uninstall.exe
	)
	set_target_properties(nsis_uninstaller PROPERTIES FOLDER "Packaging")
	add_custom_command(OUTPUT ${DATAFILE_DIR}/uninstall.exe
		DEPENDS ${_nsis_source_dir}/uninstall.nsi
			${_nsis_source_dir}/common.nsh
		COMMAND ${MAKENSIS_EXECUTABLE} ${NSIS_DEFINES}
			uninstall.nsi
		COMMAND ${DATAFILE_DIR}/uninstall_installer.exe
		COMMAND ${CMAKE_COMMAND} -E remove ${DATAFILE_DIR}/uninstall_installer.exe
		WORKING_DIRECTORY ${_nsis_source_dir}
	)
endmacro( ADD_NSIS_UNINSTALLER_TARGET )

macro( ADD_NSIS_PACKAGE_TARGET )
	set (_nsis_package ${CMAKE_BINARY_DIR}/packaging/nsis/Wireshark-$(WIRESHARK_TARGET_PLATFORM)-$(VERSION).exe)

	add_custom_target(nsis_package
		DEPENDS
			${_nsis_package}
	)
	set_target_properties(nsis_package PROPERTIES FOLDER "Packaging")
	# qt-dll-manifest.nsh. Created using Wireshark.exe.
	add_custom_command(OUTPUT ${_nsis_binary_dir}/qt-dll-manifest.nsh
		COMMAND set "PATH=%PATH%;${QT_BIN_PATH}"
		COMMAND ${POWERSHELL_COMMAND} "${_nsis_source_dir}/windeployqt-to-nsis.ps1"
			-Executable $<TARGET_FILE:wireshark>
			-FilePath ${_nsis_binary_dir}/qt-dll-manifest.nsh
	)
	# Dump the installer into ${CMAKE_CURRENT_SOURCE_DIR}/packaging/nsis to match
	# the NMake environment for now.
	add_custom_command(OUTPUT ${_nsis_package}
		DEPENDS
			${NSIS_FILES}
			${PROGLIST}
			plugins
			copy_data_files
			user_guides
			${CMAKE_BINARY_DIR}/docbook/user-guide.chm
			# We depend on the uninstaller target and not the
			# file itself, otherwise uninstall.exe will get
			# clobbered.
			nsis_uninstaller
		COMMAND ${MAKENSIS_EXECUTABLE} ${NSIS_DEFINES}
			wireshark.nsi
		WORKING_DIRECTORY ${_nsis_source_dir}
	)
endmacro( ADD_NSIS_PACKAGE_TARGET )

set(CLEAN_FILES
	all-manifest.nsh
	config.nsh
	gtk-dll-manifest.nsh
	#NEWS.txt
	qt-dll-manifest.nsh
	#user-guide.chm
	${DATAFILE_DIR}/uninstall.exe
	wireshark-$(WIRESHARK_TARGET_PLATFORM)-$(VERSION).exe
)
