cmake_minimum_required(VERSION 3.16)

project(rg_etc1 VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

add_library(rg_etc1 STATIC
    Etc1.cpp
)

add_library(rg_etc1::rg_etc1 ALIAS rg_etc1)

target_compile_features(rg_etc1 PUBLIC cxx_std_23)

target_include_directories(rg_etc1 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(NOT MSVC)
    target_compile_options(rg_etc1 PRIVATE
        -Wno-pedantic
        -Wno-class-memaccess
        -Wno-type-limits
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # The consteval sColor8ToEtcConfig*/sEtc1InverseLookup/sQuant5Tab generators
        # exceed clang's default constexpr step budget.
        target_compile_options(rg_etc1 PRIVATE -fconstexpr-steps=30000000)
    endif()
endif()

# --------------------------------------------------------------------------
# Install / find_package support (installable as rg_etc1::rg_etc1).
# --------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

install(TARGETS rg_etc1
    EXPORT rg_etc1Targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES Etc1.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT rg_etc1Targets
    FILE rg_etc1Targets.cmake
    NAMESPACE rg_etc1::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rg_etc1
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/rg_etc1ConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rg_etc1Config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/rg_etc1Config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rg_etc1
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/rg_etc1Config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/rg_etc1ConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rg_etc1
)
