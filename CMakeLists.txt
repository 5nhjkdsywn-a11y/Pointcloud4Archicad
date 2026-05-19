cmake_minimum_required (VERSION 3.16)

# ---------------------------------------------------------------------------
# Runtime-Library MUSS vor FetchContent gesetzt werden, damit Subprojekte
# (libE57Format) dieselbe Einstellung erben.
# ArchiCAD und seine DevKit-Libs nutzen /MD — Add-ons müssen übereinstimmen.
# ---------------------------------------------------------------------------
if (WIN32)
	set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
endif ()

# ---------------------------------------------------------------------------
# libE57Format — E57-Reader (automatisch heruntergeladen beim Konfigurieren)
# ---------------------------------------------------------------------------
include (FetchContent)
FetchContent_Declare (
	libE57Format
	GIT_REPOSITORY https://github.com/asmaloney/libE57Format.git
	GIT_TAG        v3.3.0
)
set (BUILD_TESTING         OFF CACHE BOOL "" FORCE)
set (E57FORMAT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable (libE57Format)

# ---------------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------------
function (SetCompilerOptions target)
	target_compile_features (${target} PUBLIC cxx_std_20)
	target_compile_options (${target} PUBLIC "$<$<CONFIG:Debug>:-DDEBUG>")
	if (WIN32)
		target_compile_options (${target} PUBLIC /W3 /WX /wd4996 /Zc:wchar_t-)
	else ()
		target_compile_options (${target} PUBLIC
			-Wall -Wextra -fvisibility=hidden
			-Wno-multichar -Wno-ctor-dtor-privacy -Wno-invalid-offsetof
			-Wno-ignored-qualifiers -Wno-reorder -Wno-overloaded-virtual
			-Wno-unused-parameter -Wno-missing-field-initializers
			-Wno-unknown-pragmas -Wno-unused-private-field
			-Wno-unused-value -Wno-switch -Wno-deprecated)
	endif ()
endfunction ()

# ---------------------------------------------------------------------------
# Alle GS-Module aus dem DevKit als Abhängigkeit hinzufügen
# ---------------------------------------------------------------------------
function (SubDirList result curdir)
	file (GLOB children RELATIVE "${curdir}" "${curdir}/*")
	set (dirs "")
	foreach (child ${children})
		if (IS_DIRECTORY "${curdir}/${child}")
			list (APPEND dirs ${child})
		endif ()
	endforeach ()
	set (${result} ${dirs} PARENT_SCOPE)
endfunction ()

function (GetModuleNameFromFolder module folder)
	if (${folder} MATCHES "^.*Lib$")
		string (REGEX REPLACE "^(.*)Lib$" "\\1" result ${folder})
	elseif (WIN32 AND ${folder} MATCHES "^Communication.*$")
		string (REGEX REPLACE "^Communication(.*)$" "Communication.\\1" result ${folder})
	else ()
		set (result ${folder})
	endif ()
	set (${module} ${result} PARENT_SCOPE)
endfunction ()

function (AddGSModule target folder)
	target_include_directories (${target} PUBLIC "${AC_API_DEVKIT_DIR}/Support/Modules/${folder}")
	set (modname "")
	GetModuleNameFromFolder (modname ${folder})
	if (WIN32)
		set (lib "${AC_API_DEVKIT_DIR}/Support/Modules/${folder}/Win/${modname}Imp.lib")
	else ()
		set (lib "${AC_API_DEVKIT_DIR}/Support/Frameworks/${modname}.framework")
	endif ()
	if (EXISTS "${lib}")
		target_link_libraries (${target} "${lib}")
	endif ()
endfunction ()

# ---------------------------------------------------------------------------
# Project setup
# ---------------------------------------------------------------------------
set (CMAKE_SUPPRESS_REGENERATION 1)
set (CMAKE_CONFIGURATION_TYPES Debug;Release;RelWithDebInfo)
if (NOT WIN32)
	set (CMAKE_OSX_DEPLOYMENT_TARGET 10.15)
	set (CMAKE_OSX_ARCHITECTURES "x86_64" CACHE INTERNAL "" FORCE)
	set (CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "YES")
endif ()

if (WIN32)
	set (AC_API_DEVKIT_DIR "${CMAKE_CURRENT_LIST_DIR}/../windows/API.Development.Kit.WIN.26.7000"
	     CACHE PATH "ArchiCAD API DevKit directory")
else ()
	set (AC_API_DEVKIT_DIR "${CMAKE_CURRENT_LIST_DIR}/../macos/archicad-api-devkit-26.7000/API"
	     CACHE PATH "ArchiCAD API DevKit directory")
endif ()
set (AC_ADDON_LANGUAGE "INT" CACHE STRING "Add-On language code")
set (PW_VERSION "0.1.1" CACHE STRING "PointCloud Add-On version string")

# Detect ArchiCAD version from DevKit headers, append to addon name
set (ACAPINC "${AC_API_DEVKIT_DIR}/Support/Inc/ACAPinc.h")
if (EXISTS "${ACAPINC}")
	file (READ "${ACAPINC}" ACAPIncContent)
	string (REGEX MATCHALL "#define[ \t]+ServerMainVers_([0-9][0-9])" _vers "${ACAPIncContent}")
	set (AC_VERSION "${CMAKE_MATCH_1}")
	if (WIN32)
		set (AC_ADDON_NAME "PointCloudDensity_x64_${AC_VERSION}" CACHE STRING "Add-On name" FORCE)
	else ()
		set (AC_ADDON_NAME "PointCloudDensity_${CMAKE_OSX_ARCHITECTURES}_${AC_VERSION}" CACHE STRING "Add-On name" FORCE)
	endif ()
	message (STATUS "ArchiCAD version: ${AC_VERSION}  Addon: ${AC_ADDON_NAME}  DevKit: ${AC_API_DEVKIT_DIR}")
	configure_file (
		"${CMAKE_CURRENT_LIST_DIR}/RINT/PointCloudDensity.grc.in"
		"${CMAKE_BINARY_DIR}/PointCloudDensity_generated.grc"
		@ONLY)
	if (WIN32)
		configure_file (
			"${CMAKE_CURRENT_LIST_DIR}/RFIX.win/PointCloudDensity.rc2.in"
			"${CMAKE_CURRENT_LIST_DIR}/RFIX.win/PointCloudDensity.rc2"
			@ONLY)
	endif ()
else ()
	message (FATAL_ERROR "DevKit not found at ${AC_API_DEVKIT_DIR}")
endif ()

if (WIN32)
	add_definitions (-DUNICODE -D_UNICODE)
	# AC27+: iterator debug level must be 0 to match ArchiCAD's non-debug STL
	if (AC_VERSION GREATER_EQUAL 27)
		add_definitions (-D_ITERATOR_DEBUG_LEVEL=0)
	endif ()
	# AC28+: GS_WIN required by newer GS module headers
	if (AC_VERSION GREATER_EQUAL 28)
		add_definitions (-DGS_WIN)
	endif ()
else ()
	add_definitions (-Dmacintosh=1)
	# AC28+: GS_MAC required by newer GS module headers
	if (AC_VERSION GREATER_EQUAL 28)
		add_definitions (-DGS_MAC)
	endif ()
endif ()
add_definitions (-DACExtension)
add_definitions (-DPW_VERSION="${PW_VERSION}")

project (${AC_ADDON_NAME})

# ---------------------------------------------------------------------------
# Resource compilation
# ---------------------------------------------------------------------------
set (ResourceObjectsDir "${CMAKE_BINARY_DIR}/ResourceObjects")
set (AddOnResourcesFolder "${CMAKE_CURRENT_LIST_DIR}")
set (AddOnSourcesFolder   "${CMAKE_CURRENT_LIST_DIR}/src")
set (APIDevKitToolsDir    "${AC_API_DEVKIT_DIR}/Support/Tools")

file (GLOB AddOnImageFiles
	"${AddOnResourcesFolder}/RFIX/Images/*.svg")

if (WIN32)
	file (GLOB AddOnResourceFiles
		"${AddOnResourcesFolder}/R${AC_ADDON_LANGUAGE}/*.grc"
		"${AddOnResourcesFolder}/RFIX/*.grc"
		"${AddOnResourcesFolder}/RFIX.win/*.rc2")
else ()
	file (GLOB AddOnResourceFiles
		"${AddOnResourcesFolder}/R${AC_ADDON_LANGUAGE}/*.grc"
		"${AddOnResourcesFolder}/RFIX/*.grc"
		"${AddOnResourcesFolder}/RFIX.mac/*.plist")
endif ()

source_group ("Images" FILES ${AddOnImageFiles})
source_group ("Resources" FILES ${AddOnResourceFiles})

add_custom_target (AddOnResources ALL
	DEPENDS "${ResourceObjectsDir}/AddOnResources.stamp"
	SOURCES ${AddOnResourceFiles} ${AddOnImageFiles})

if (WIN32)
	add_custom_command (
		OUTPUT  "${ResourceObjectsDir}/AddOnResources.stamp"
		DEPENDS ${AddOnResourceFiles} ${AddOnImageFiles}
		COMMENT "Compiling resources..."
		COMMAND ${CMAKE_COMMAND} -E make_directory "${ResourceObjectsDir}"
		COMMAND python "${APIDevKitToolsDir}/CompileResources.py"
		        "${AC_ADDON_LANGUAGE}" "${AC_API_DEVKIT_DIR}"
		        "${AddOnSourcesFolder}" "${AddOnResourcesFolder}"
		        "${ResourceObjectsDir}" "${ResourceObjectsDir}/${AC_ADDON_NAME}.res"
		COMMAND ${CMAKE_COMMAND} -E touch "${ResourceObjectsDir}/AddOnResources.stamp")
else ()
	add_custom_command (
		OUTPUT  "${ResourceObjectsDir}/AddOnResources.stamp"
		DEPENDS ${AddOnResourceFiles} ${AddOnImageFiles}
		COMMENT "Compiling resources..."
		COMMAND ${CMAKE_COMMAND} -E make_directory "${ResourceObjectsDir}"
		COMMAND ${CMAKE_COMMAND} -E copy
		        "${CMAKE_BINARY_DIR}/PointCloudDensity_generated.grc"
		        "${AddOnResourcesFolder}/RINT/PointCloudDensity.grc"
		COMMAND python3 "${APIDevKitToolsDir}/CompileResources.py"
		        "${AC_ADDON_LANGUAGE}" "${AC_API_DEVKIT_DIR}"
		        "${AddOnSourcesFolder}" "${AddOnResourcesFolder}"
		        "${ResourceObjectsDir}"
		        "${CMAKE_BINARY_DIR}/$<CONFIG>/${AC_ADDON_NAME}.bundle/Contents/Resources"
		COMMAND ${CMAKE_COMMAND} -E copy
		        "${AC_API_DEVKIT_DIR}/Support/Inc/PkgInfo"
		        "${CMAKE_BINARY_DIR}/$<CONFIG>/${AC_ADDON_NAME}.bundle/Contents/PkgInfo"
		COMMAND ${CMAKE_COMMAND} -E touch "${ResourceObjectsDir}/AddOnResources.stamp")
endif ()

# ---------------------------------------------------------------------------
# AddOn target
# ---------------------------------------------------------------------------
file (GLOB AddOnSourceFiles "${AddOnSourcesFolder}/*.cpp" "${AddOnSourcesFolder}/*.c")
file (GLOB AddOnHeaderFiles "${AddOnSourcesFolder}/*.h"   "${AddOnSourcesFolder}/*.hpp")
source_group ("Sources" FILES ${AddOnSourceFiles} ${AddOnHeaderFiles})

if (WIN32)
	add_library (AddOn SHARED  ${AddOnSourceFiles} ${AddOnHeaderFiles})
else ()
	add_library (AddOn MODULE  ${AddOnSourceFiles} ${AddOnHeaderFiles})
endif ()

set_target_properties (AddOn PROPERTIES OUTPUT_NAME "${AC_ADDON_NAME}")
if (WIN32)
	set_target_properties (AddOn PROPERTIES SUFFIX ".apx")
else ()
	set_target_properties (AddOn PROPERTIES BUNDLE TRUE)
	set_target_properties (AddOn PROPERTIES
		MACOSX_BUNDLE_INFO_PLIST "${AddOnResourcesFolder}/RFIX.mac/Info.plist")
	set_target_properties (AddOn PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
endif ()

target_include_directories (AddOn PUBLIC
	"${AddOnSourcesFolder}"
	"${AC_API_DEVKIT_DIR}/Support/Inc")

if (WIN32)
	# AC26 has libs in Support/Lib/Win/, AC27+ directly in Support/Lib/
	if (EXISTS "${AC_API_DEVKIT_DIR}/Support/Lib/Win/ACAP_STAT.lib")
		set (ACAP_LIB_DIR "${AC_API_DEVKIT_DIR}/Support/Lib/Win")
	else ()
		set (ACAP_LIB_DIR "${AC_API_DEVKIT_DIR}/Support/Lib")
	endif ()
	target_link_options (AddOn PUBLIC
		"${ResourceObjectsDir}/${AC_ADDON_NAME}.res"
		/export:GetExportedFuncAddrs,@1 /export:SetImportedFuncAddrs,@2)
	# Always use the release ACAP_STAT.lib — DevKit AC27+ ships only this lib,
	# and MultiThreadedDLL (/MD) must match across all targets incl. ACAP_STAT.lib
	target_link_libraries (AddOn
		"${ACAP_LIB_DIR}/ACAP_STAT.lib"
		E57Format
		advapi32)
else ()
	find_library (CocoaFramework Cocoa)
	# AC26: Lib/Mactel/  |  AC27+: Lib/ directly
	if (EXISTS "${AC_API_DEVKIT_DIR}/Support/Lib/Mactel/libACAP_STAT.a")
		set (ACAP_STAT_LIB "${AC_API_DEVKIT_DIR}/Support/Lib/Mactel/libACAP_STAT.a")
	else ()
		set (ACAP_STAT_LIB "${AC_API_DEVKIT_DIR}/Support/Lib/libACAP_STAT.a")
	endif ()
	target_link_libraries (AddOn
		"${ACAP_STAT_LIB}"
		${CocoaFramework}
		E57Format)
endif ()

SetCompilerOptions (AddOn)
add_dependencies  (AddOn AddOnResources)

# Alle verfügbaren GS-Module aus dem DevKit einbinden
set (GSModules "")
SubDirList (GSModules "${AC_API_DEVKIT_DIR}/Support/Modules")
foreach (mod ${GSModules})
	AddGSModule (AddOn ${mod})
endforeach ()
