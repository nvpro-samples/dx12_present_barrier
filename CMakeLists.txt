cmake_minimum_required(VERSION 3.5)

get_filename_component(PROJNAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)
Project(${PROJNAME} VERSION 1.4)
Message(STATUS "-------------------------------")
Message(STATUS "Processing Project ${PROJNAME}:")

# ####################################################################################
# look for nvpro_core 1) as a sub-folder 2) at some other locations
# this cannot be put anywhere else since we still didn't find setup.cmake yet
#
if(NOT BASE_DIRECTORY)
  find_path(BASE_DIRECTORY
    NAMES nvpro_core/cmake/setup.cmake
    PATHS ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../..
    REQUIRED
    DOC "Directory containing nvpro_core"
  )
endif()

if(EXISTS ${BASE_DIRECTORY}/nvpro_core/cmake/setup.cmake)
  include(${BASE_DIRECTORY}/nvpro_core/cmake/setup.cmake)
else()
  message(FATAL_ERROR "could not find base directory, please set BASE_DIRECTORY to folder containing nvpro_core")
endif()

_add_project_definitions(${PROJNAME})

# ####################################################################################
# additions from packages needed for this sample
# add refs  in LIBRARIES_OPTIMIZED
# add refs  in LIBRARIES_DEBUG
# add files in PACKAGE_SOURCE_FILES
set(EXENAME ${PROJNAME})
_add_package_DirectX12()
_add_package_ImGUI()
_add_package_NVAPI()

# ####################################################################################
# process the rest of some cmake code that needs to be done *after* the packages add
_add_nvpro_core_lib()

# ####################################################################################
# Source files for this project
#
file(GLOB SOURCE_FILES *.cpp *.hpp *.inl *.h *.c)
file(GLOB HLSL_PIXEL_SHADER_FILES shaders/ps.hlsl shaders/gui_ps.hlsl)
file(GLOB HLSL_VERTEX_SHADER_FILES shaders/indicator_vs.hlsl shaders/line_vs.hlsl shaders/gui_vs.hlsl)

# ####################################################################################
# Executable
#
set_property(SOURCE ${HLSL_PIXEL_SHADER_FILES} PROPERTY VS_SHADER_TYPE Pixel)
set_property(SOURCE ${HLSL_PIXEL_SHADER_FILES} PROPERTY VS_SHADER_MODEL 5.0)
set_property(SOURCE ${HLSL_VERTEX_SHADER_FILES} PROPERTY VS_SHADER_TYPE Vertex)
set_property(SOURCE ${HLSL_VERTEX_SHADER_FILES} PROPERTY VS_SHADER_MODEL 5.0)

add_executable(${EXENAME} ${SOURCE_FILES} ${COMMON_SOURCE_FILES} ${PACKAGE_SOURCE_FILES} ${HLSL_PIXEL_SHADER_FILES} ${HLSL_VERTEX_SHADER_FILES})

find_package(Git)

set(GIT_HASH custom)
set(GIT_UNCOMMITTED_CHANGES 0)

if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --is-inside-work-tree
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE IS_GIT_REPO
    OUTPUT_QUIET
    ERROR_QUIET
  )

  if(IS_GIT_REPO EQUAL 0)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} log -1 --format=%h
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      OUTPUT_VARIABLE GIT_HASH
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    execute_process(
      COMMAND ${GIT_EXECUTABLE} diff --quiet
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      RESULT_VARIABLE GIT_UNCOMMITTED_CHANGES
    )
  endif()
endif()

target_include_directories(${EXENAME} PRIVATE ${CMAKE_BINARY_DIR})
configure_file(version.h.in version.h)

# _set_subsystem_console(${EXENAME})

# ####################################################################################
# common source code needed for this sample
#
source_group("" FILES
  ${SOURCE_FILES}
)
source_group(shaders FILES
  ${HLSL_PIXEL_SHADER_FILES}
  ${HLSL_VERTEX_SHADER_FILES}
)
source_group(resources FILES
  ${COMMON_SOURCE_FILES}
  ${PACKAGE_SOURCE_FILES}
)

if(UNIX)
  set(UNIXLINKLIBS dl pthread)
else()
  set(UNIXLINKLIBS)
endif()

# ####################################################################################
# Linkage
#
target_link_libraries(${EXENAME} ${PLATFORM_LIBRARIES} nvpro_core)

foreach(DEBUGLIB ${LIBRARIES_DEBUG})
  target_link_libraries(${EXENAME} debug ${DEBUGLIB})
endforeach(DEBUGLIB)

foreach(RELEASELIB ${LIBRARIES_OPTIMIZED})
  target_link_libraries(${EXENAME} optimized ${RELEASELIB})
endforeach(RELEASELIB)

# ####################################################################################
# copies binaries that need to be put next to the exe files (ZLib, etc.)
#
_finalize_target(${EXENAME})
