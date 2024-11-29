cmake_minimum_required(VERSION 3.26)
message(STATUS "CMake version: ${CMAKE_VERSION}")

project(ffmpeg_demo C CXX)
message(STATUS "PROJECT NAME: ${PROJECT_NAME} ${PROJECT_SOURCE_DIR}")

set(CMAKE_CUDA_ARCHITECTURES "86")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
#debug or release
set(CMAKE_DEBUG_POSTFIX d) 
set(CMAKE_BUILD_TYPE "Debug")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    #debug
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -g3 -O0 -ggdb3 -gdwarf-3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -g3 -O0 -ggdb3 -gdwarf-3")
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    #release with debug info 
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O2")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O2")
else()
    #release
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}  -O2 -s")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -s")
endif()

set(CMAKE_CXX_STANDARD 11) 
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
set(CMAKE_C_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
set(EXECUTABLE_OUTPUT_PATH "../")


set(FFMPEG_PATH "./ffmpeh6.0")
# set(BOOST_PATH "/home/peihuang/SoftInstall/boost_1_86_0")
include_directories(
    ${FFMPEG_PATH}/include
    # ${BOOST_PATH}/include
)

link_directories(
    ${FFMPEG_PATH}/lib
    # ${BOOST_PATH}/lib
)
file(GLOB FFMPEG_LIBS "${FFMPEG_PATH}/lib/*.so")
# file(GLOB BOOST_LIBS "${BOOST_PATH}/lib/*.so")
set(third_lib "")
list(APPEND third_lib ${FFMPEG_LIBS} ${BOOST_LIBS})

file(GLOB CPP_FILES "*.c")
message(STATUS "CPP FILES: ${CPP_FILES}")
foreach(CPP_FILE ${CPP_FILES})
    # 获取文件名，不包含路径和扩展名
    get_filename_component(EXE_NAME ${CPP_FILE} NAME_WE)
    # 为每个 .cpp 文件创建一个可执行文件
    add_executable(${EXE_NAME} ${CPP_FILE})
    target_link_libraries(${EXE_NAME} ${third_lib} )
    target_link_libraries(${EXE_NAME} m)
endforeach()

