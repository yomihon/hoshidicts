cmake_minimum_required(VERSION 3.22.1)
cmake_policy(SET CMP0077 NEW)

project(hoshidicts LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "")
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "")
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "")
set(ZSTD_BUILD_STATIC ON CACHE BOOL "")
set(glaze_DISABLE_SIMD_WHEN_SUPPORTED ON CACHE BOOL "")

function(resolve_dependency_dir out_var local_dir repo tag)
    set(local_path "${CMAKE_CURRENT_SOURCE_DIR}/${local_dir}")
    file(GLOB local_entries LIST_DIRECTORIES true "${local_path}/*")
    if(local_entries)
        set(${out_var} "${local_path}" PARENT_SCOPE)
        return()
    endif()

    string(MAKE_C_IDENTIFIER "${local_dir}" dep_name)
    string(TOLOWER "${dep_name}" dep_name)
    FetchContent_Declare(${dep_name}
        GIT_REPOSITORY ${repo}
        GIT_TAG ${tag}
    )
    FetchContent_GetProperties(${dep_name})
    if(NOT ${dep_name}_POPULATED)
        FetchContent_Populate(${dep_name})
    endif()

    set(${out_var} "${${dep_name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(add_dependency_subdirectory local_dir repo tag)
    resolve_dependency_dir(dep_source_dir "${local_dir}" "${repo}" "${tag}")
    string(MAKE_C_IDENTIFIER "${local_dir}" dep_name)
    string(TOLOWER "${dep_name}" dep_name)
    add_subdirectory("${dep_source_dir}" "${CMAKE_BINARY_DIR}/deps/${dep_name}")
endfunction()

add_dependency_subdirectory("external/zip" "https://github.com/kuba--/zip" "296ff2421b6bdb82575dd943fb8b5d7d5d443903")
add_dependency_subdirectory("external/glaze" "https://github.com/stephenberry/glaze" "be5159d80c480ec0d97db40f685983e2f7ade2d3")
add_dependency_subdirectory("external/zstd" "https://github.com/facebook/zstd.git" "0532fe3e8ac1caf48f11603847ac3176064189d1")
add_dependency_subdirectory("external/unordered_dense" "https://github.com/martinus/unordered_dense.git" "7b55cab8418da1603496462ce3ccdb4cb1dc3368")
resolve_dependency_dir(UTFCPP_SOURCE_DIR "external/utfcpp" "https://github.com/nemtrif/utfcpp.git" "cfc9112cee3e817e8b72948a675f78479546f0cf")
resolve_dependency_dir(XXHASH_SOURCE_DIR "external/xxHash" "https://github.com/Cyan4973/xxHash.git" "82cead715cbfddd9e6093db8df95155077ce6e64")

target_compile_definitions(zip PRIVATE
    MINIZ_DISABLE_ZIP_READER_CRC32_CHECKS
    MINIZ_NO_TIME
)

target_compile_options(zip PRIVATE
    -Wno-unknown-warning-option
    -Wno-error=stringop-truncation
)

add_library(hoshidicts
    src/hash/hash.cpp
    src/importer.cpp
    src/json/yomitan_parser.cpp
    src/text_processor/text_processor.cpp
    src/deconjugator/deconjugator.cpp
    src/query.cpp
    src/lookup.cpp
)

target_compile_definitions(hoshidicts PRIVATE GLZ_DISABLE_SIMD)

target_include_directories(hoshidicts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${UTFCPP_SOURCE_DIR}/source
    ${XXHASH_SOURCE_DIR}
)

target_link_libraries(hoshidicts PRIVATE
    zip
    glaze::glaze
    libzstd_static
    unordered_dense
)

if(ANDROID)
    find_library(log-lib log)
    add_library(hoshidicts_jni SHARED hoshidicts_jni.cpp)
    target_link_libraries(hoshidicts_jni PRIVATE hoshidicts ${log-lib})
else()
    add_executable(hoshidicts-cli
        cli/main.cpp
    )

    target_compile_features(hoshidicts-cli PRIVATE cxx_std_23)
    target_link_libraries(hoshidicts-cli PRIVATE
        hoshidicts
    )

    add_executable(benchmark-import
        benchmark/import.cpp
    )

    target_compile_features(benchmark-import PRIVATE cxx_std_23)
    target_link_libraries(benchmark-import PRIVATE
        hoshidicts
    )

    add_executable(benchmark-lookup
        benchmark/lookup.cpp
    )

    target_compile_features(benchmark-lookup PRIVATE cxx_std_23)
    target_link_libraries(benchmark-lookup PRIVATE
        hoshidicts
    )
endif()
