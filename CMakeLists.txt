cmake_minimum_required(VERSION 3.15)

project(SlimeVR-Feeder-App LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

find_package(fmt CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(OpenXR CONFIG REQUIRED)

set(protos_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/protos)
file(MAKE_DIRECTORY "${protos_OUTPUT_DIR}")

# Project
add_executable("${PROJECT_NAME}" "src/main.cpp" "src/bridge.cpp" "src/bridge.hpp" "ProtobufMessages.proto")
target_compile_definitions("${PROJECT_NAME}" PRIVATE "XR_USE_TIMESPEC=1")
target_link_libraries("${PROJECT_NAME}" PRIVATE fmt::fmt protobuf::libprotobuf openxr_loader)
protobuf_generate(TARGET "${PROJECT_NAME}" LANGUAGE cpp PROTOC_OUT_DIR ${protos_OUTPUT_DIR})
target_include_directories("${PROJECT_NAME}" PUBLIC ${protos_OUTPUT_DIR} PRIVATE ${CMAKE_CURRENT_BINARY_DIR} "include/")

# IDE Config
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Header Files" FILES ${HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source Files" FILES ${SOURCES})

add_custom_target(
    version
    COMMAND ${CMAKE_COMMAND}
        -Dlocal_dir="${CMAKE_CURRENT_SOURCE_DIR}"
        -Doutput_dir="${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gitversion.cmake"
)
add_dependencies("${PROJECT_NAME}" version)

install(DIRECTORY "${PROJECT_SOURCE_DIR}/bindings"
    DESTINATION "."
)
install(FILES "${PROJECT_SOURCE_DIR}/manifest.vrmanifest"
    DESTINATION "."
)

install(TARGETS "${PROJECT_NAME}"
    RUNTIME
    DESTINATION .
)

set(CPACK_SYSTEM_NAME ${CMAKE_SYSTEM_NAME})
if(${CPACK_SYSTEM_NAME} MATCHES Windows)
  if(CMAKE_CL_64)
    set(CPACK_SYSTEM_NAME win64)
    set(CPACK_IFW_TARGET_DIRECTORY "@RootDir@/Program Files/${CMAKE_PROJECT_NAME}")
  else()
    set(CPACK_SYSTEM_NAME win32)
  endif()
endif()

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_FILE_NAME ${CPACK_PROJECT_NAME}-${CPACK_SYSTEM_NAME})
set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_VERBATIM_VARIABLES YES)
include(CPack)
