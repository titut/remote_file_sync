cmake_minimum_required(VERSION 3.5)

# Definitions
set(PROJECT_NAME remote_file_sync)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "./bin")

#project(cmake_c_template)
project(${PROJECT_NAME})
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99")

# Include paths
set(INCLUDE_DIRS ${INCLUDE_DIRS} ${PROJECT_SOURCE_DIR}/include)

# Assign the include directories
include_directories(${INCLUDE_DIRS})

# Find library sources
#file(GLOB_RECURSE LIB_SRC_FILES include/*/*.c)

# Build static/shared library
#add_library(${PROJECT_NAME}_LIB STATIC ${LIB_SRC_FILES})

# Install library
#install(TARGETS ${PROJECT_NAME}_LIB DESTINATION lib)

# Install library headers
file(GLOB_RECURSE LIB_INC_FILES include/*.h)
install(FILES ${LIB_INC_FILES} DESTINATION include)

set(SOURCE_FILES client/file_watcher.c include/file_watcher.h)
add_executable(remote_file_sync ${SOURCE_FILES})
#target_link_libraries(${PROJECT_NAME} ${PROJECT_NAME}_LIB)