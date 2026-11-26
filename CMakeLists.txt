cmake_minimum_required(VERSION 3.22)

project(cop LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The tool is generic: it needs only the shared-page transport, never the product
# contract.  Both dependencies are provided as ordinary checkouts so the image's
# vendor build can point at the exact headers the firmware was built with.
set(COP_LIBXR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libxr" CACHE PATH
    "LibXR checkout (Linux backend)")
set(COP_SHARED_PAGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/shared-page-xr" CACHE PATH
    "shared-page-xr checkout")
option(COP_STATIC "Link a static binary (cross builds for the board)" OFF)
option(COP_BUILD_TESTS "Build the host CLI tests" ON)

foreach(dependency "${COP_LIBXR_DIR}" "${COP_SHARED_PAGE_DIR}")
  if(NOT EXISTS "${dependency}/CMakeLists.txt")
    message(FATAL_ERROR
      "missing dependency: ${dependency}.  Set COP_LIBXR_DIR / COP_SHARED_PAGE_DIR "
      "to the checkouts (the image's vendor build points them at the firmware tree).")
  endif()
endforeach()

set(LIBXR_SYSTEM linux CACHE STRING "" FORCE)
# `cop` uses only the page transport and the POSIX UIO/mailbox headers; pulling the
# Linux GPIO/udev driver would add a dependency the board does not need.
set(LIBXR_DRIVER "" CACHE STRING "" FORCE)
set(LIBXR_SINGLE_CORE ON CACHE BOOL "" FORCE)

# Configure LibXR for its public include directories only.  The tool does not link
# `xr`: the page transport is header-only apart from `shared_page.cpp`, and compiling
# that file directly keeps the cross build free of udev/GPIO.
add_subdirectory("${COP_LIBXR_DIR}" libxr EXCLUDE_FROM_ALL)
get_target_property(COP_LIBXR_INCLUDE_DIRS xr INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(COP_LIBXR_DEFINITIONS xr INTERFACE_COMPILE_DEFINITIONS)

add_library(cop_core STATIC
  "${COP_SHARED_PAGE_DIR}/shared_page.cpp"
  "${COP_LIBXR_DIR}/src/utils/crc_o3.cpp"
)
target_compile_features(cop_core PUBLIC cxx_std_20)
target_include_directories(cop_core PUBLIC
  "${COP_SHARED_PAGE_DIR}"
  ${COP_LIBXR_INCLUDE_DIRS}
)
target_compile_definitions(cop_core PUBLIC ${COP_LIBXR_DEFINITIONS})

add_executable(cop
  src/main.cpp
  src/commands.cpp
)
target_compile_features(cop PRIVATE cxx_std_20)
target_include_directories(cop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cop PRIVATE cop_core)
target_compile_options(cop PRIVATE -Wall -Wextra -Wpedantic)

if(COP_STATIC)
  target_link_options(cop PRIVATE -static -s)
endif()

if(COP_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_executable(cop_fixture test/fixture.cpp)
  target_compile_features(cop_fixture PRIVATE cxx_std_20)
  target_link_libraries(cop_fixture PRIVATE cop_core)
  target_compile_options(cop_fixture PRIVATE -Wall -Wextra -Wpedantic)

  set(COP_TEST_PAGE "${CMAKE_CURRENT_BINARY_DIR}/cop-test-page.bin")
  add_test(NAME cop_fixture COMMAND cop_fixture "${COP_TEST_PAGE}")
  set_tests_properties(cop_fixture PROPERTIES FIXTURES_SETUP cop_page)

  add_test(NAME cop_status COMMAND cop --page "${COP_TEST_PAGE}" status)
  set_tests_properties(cop_status PROPERTIES
    FIXTURES_REQUIRED cop_page
    PASS_REGULAR_EXPRESSION "FORMATTED")

  add_test(NAME cop_dump COMMAND cop --page "${COP_TEST_PAGE}" dump --offset 0 --bytes 16)
  set_tests_properties(cop_dump PROPERTIES
    FIXTURES_REQUIRED cop_page
    PASS_REGULAR_EXPRESSION "61 64 50 31")

  add_test(NAME cop_rejects_contract
           COMMAND cop --page "${COP_TEST_PAGE}" status --contract anything)
  set_tests_properties(cop_rejects_contract PROPERTIES
    FIXTURES_REQUIRED cop_page
    WILL_FAIL TRUE)
endif()
