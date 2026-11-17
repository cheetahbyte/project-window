cmake_minimum_required(VERSION 3.25)
project(project_window LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)

option(PW_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(PW_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()
set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fobjc-arc")

add_library(protocol STATIC src/protocol/transport.cpp)
target_include_directories(protocol PUBLIC src)

add_library(clientkit STATIC src/clientkit/client.cpp)
target_link_libraries(clientkit PUBLIC protocol "-framework IOSurface" "-framework CoreFoundation")

set(SHADER_AIR ${CMAKE_BINARY_DIR}/composite.air)
set(SHADER_LIB ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/composite.metallib)
add_custom_command(
  OUTPUT ${SHADER_LIB}
  COMMAND xcrun metal -c ${CMAKE_SOURCE_DIR}/shaders/composite.metal -o ${SHADER_AIR}
  COMMAND xcrun metallib ${SHADER_AIR} -o ${SHADER_LIB}
  DEPENDS ${CMAKE_SOURCE_DIR}/shaders/composite.metal
  VERBATIM)
add_custom_target(shaders DEPENDS ${SHADER_LIB})

add_executable(compositor
  src/compositor/main.mm
  src/compositor/server.mm
  src/compositor/scene.cpp)
add_dependencies(compositor shaders)
target_link_libraries(compositor PRIVATE protocol
  "-framework AppKit" "-framework Metal" "-framework MetalKit"
  "-framework QuartzCore" "-framework IOSurface")

add_executable(demo-client src/demo-client/main.cpp)
target_link_libraries(demo-client PRIVATE clientkit)

add_executable(capture-scene src/capture-scene/main.cpp)
target_link_libraries(capture-scene PRIVATE "-framework CoreGraphics" "-framework CoreFoundation")

add_executable(bench src/bench/main.cpp)
