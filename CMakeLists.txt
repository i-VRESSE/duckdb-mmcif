cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME mmcif)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)
set(MMCIF_GEN "${CMAKE_CURRENT_BINARY_DIR}/mmcif-gen")
file(MAKE_DIRECTORY ${MMCIF_GEN})
include_directories(${MMCIF_GEN})

# No external deps: read/write use the no-deps MmcifIndex/MmcifWriteStore core.

# Embed the checked-in gzip'd dictionary artifacts into mmcif_dict_data.hpp so the
# extension has no runtime data-file dependency (see .scratch/mmcif-extension/map.md).
# The header is generated into ${MMCIF_GEN}, which is on the include path above.
add_custom_command(
  OUTPUT ${MMCIF_GEN}/mmcif_dict_data.hpp
  COMMAND ${CMAKE_COMMAND} -E env python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_artifact.py
          ${MMCIF_GEN}/mmcif_dict_data.hpp
          ${CMAKE_CURRENT_SOURCE_DIR}/dict/mmcif_pdbx_v50_type_index.tsv.gz MMCIFF_TYPE_INDEX_GZ
          ${CMAKE_CURRENT_SOURCE_DIR}/dict/mmcif_pdbx_v50_relationships.tsv.gz MMCIFF_RELATIONSHIPS_GZ
  DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/dict/mmcif_pdbx_v50_type_index.tsv.gz
    ${CMAKE_CURRENT_SOURCE_DIR}/dict/mmcif_pdbx_v50_relationships.tsv.gz
  COMMENT "embedding mmcif dictionary artifacts"
  VERBATIM
)

# Make the extension targets build the generated dict-data header.
add_custom_target(mmciff_dict_data DEPENDS ${MMCIF_GEN}/mmcif_dict_data.hpp)

set(EXTENSION_SOURCES
  src/mmcif_extension.cpp
  src/mmcif_index.cpp
  src/mmcif_write_store.cpp
  src/mmcif_dictionary.cpp
  src/mmcif_writer.cpp
  src/mmcif_file.cpp
  src/mmcif_table_functions.cpp
  src/mmcif_catalog.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

add_dependencies(${EXTENSION_NAME} mmciff_dict_data)
add_dependencies(${LOADABLE_EXTENSION_NAME} mmciff_dict_data)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
