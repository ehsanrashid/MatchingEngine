cmake_minimum_required(VERSION 3.31)
project(AeronMatchingEngine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set path to Aeron
set(AERON_DIR ${CMAKE_SOURCE_DIR}/external/aeron)
set(AERON_INCLUDE_DIR ${AERON_DIR}/aeron-client/src/main/cpp)
set(AERON_LIB_DIR ${AERON_DIR}/build/lib)

# Try to find the exact library file
find_library(AERON_CLIENT_LIB 
    NAMES aeron_client libaeron_client
    PATHS ${AERON_LIB_DIR} ${AERON_LIB_DIR}/lib
    NO_DEFAULT_PATH
)

if(NOT AERON_CLIENT_LIB)
    message(WARNING "Could not find aeron_client library. Trying with link_directories approach.")
    link_directories(${AERON_LIB_DIR})
    link_directories(${AERON_LIB_DIR}/lib)
    set(AERON_CLIENT_LIB aeron_client)
endif()

# Print some information for debugging
message(STATUS "Aeron include directory: ${AERON_INCLUDE_DIR}")
message(STATUS "Aeron library directory: ${AERON_LIB_DIR}")
message(STATUS "Aeron client library: ${AERON_CLIENT_LIB}")

# Includes and link dirs
include_directories(${AERON_INCLUDE_DIR})
link_directories(${AERON_LIB_DIR})

# Source files
add_executable(matching_engine Main.cpp)
set_target_properties(matching_engine PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
# Link libraries
target_link_libraries(matching_engine 
    ${AERON_CLIENT_LIB}
    pthread
)

# Add some platform-specific libraries if needed
if(UNIX AND NOT APPLE)
    target_link_libraries(matching_engine rt dl)
elseif(APPLE)
    target_link_libraries(matching_engine dl)
endif()


# Optional: Add installation target
install(TARGETS matching_engine DESTINATION bin)
