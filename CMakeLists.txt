cmake_minimum_required(VERSION 3.16)

project(ExETC1 VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

add_library(ExETC1 STATIC
    Etc1.cpp
)

add_library(ExETC1::ExETC1 ALIAS ExETC1)

target_compile_features(ExETC1 PUBLIC cxx_std_23)

target_include_directories(ExETC1 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(NOT MSVC)
    target_compile_options(ExETC1 PRIVATE
        -Wno-pedantic
        -Wno-class-memaccess
        -Wno-type-limits
        -Wno-ignored-attributes # std::array<__m128i/__m256i> in the SSE paths
    )
endif()

# --------------------------------------------------------------------------
# Install / find_package support (installable as ExETC1::ExETC1).
# --------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

install(TARGETS ExETC1
    EXPORT ExETC1Targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES Etc1.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ExETC1Targets
    FILE ExETC1Targets.cmake
    NAMESPACE ExETC1::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ExETC1
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ExETC1ConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExETC1Config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ExETC1Config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ExETC1
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ExETC1Config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ExETC1ConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ExETC1
)
