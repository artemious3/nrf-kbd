# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "D:/WORK_NRF/test_kbd/test1")
  file(MAKE_DIRECTORY "D:/WORK_NRF/test_kbd/test1")
endif()
file(MAKE_DIRECTORY
  "D:/WORK_NRF/test_kbd/test1/build/test1"
  "D:/WORK_NRF/test_kbd/test1/build/_sysbuild/sysbuild/images/test1-prefix"
  "D:/WORK_NRF/test_kbd/test1/build/_sysbuild/sysbuild/images/test1-prefix/tmp"
  "D:/WORK_NRF/test_kbd/test1/build/_sysbuild/sysbuild/images/test1-prefix/src/test1-stamp"
  "D:/WORK_NRF/test_kbd/test1/build/_sysbuild/sysbuild/images/test1-prefix/src"
  "D:/WORK_NRF/test_kbd/test1/build/_sysbuild/sysbuild/images/test1-prefix/src/test1-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/WORK_NRF/test_kbd/test1/build/_sysbuild/sysbuild/images/test1-prefix/src/test1-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/WORK_NRF/test_kbd/test1/build/_sysbuild/sysbuild/images/test1-prefix/src/test1-stamp${cfgdir}") # cfgdir has leading slash
endif()
