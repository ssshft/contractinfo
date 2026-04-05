cmake_minimum_required(VERSION 3.1)
project(ContractInfo)

# 设置编译器 c++17 required
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# set(CMAKE_BUILD_TYPE "Debug")
set(CMAKE_BUILD_TYPE "Release")
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -fPIC -Wall -g -ggdb")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -fPIC -DABI -DNDEBUG -DOPENBLAS  -O3 -Wall -flto -fuse-linker-plugin -minline-all-stringops -malign-double -ffast-math -march=native -ffunction-sections -fdata-sections -fmessage-length=0 -msse -msse2 -msse3 -funroll-loops")

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/bin)


file(READ /inc/version.inc COMPILE_VERSION LIMIT 8 OFFSET 33)
message(${COMPILE_VERSION})
set(COMPILE_PATH ${COMPILE_VERSION})

set(3RD_PATH /opt/version/3rd)

#include
set(INC_DIR              ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(SYSTEM_INC_DIR          ${COMPILE_PATH}/include)
set(FMT_INCLUDE_DIR          ${3RD_PATH}/fmt/include)


#link
set(FMT_LIBRARY        ${3RD_PATH}/fmt/lib)


aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src       DIRSRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/base     DIRSRCS_BASE)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/binance   DIR_BINANCE_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/okx     DIR_OKX_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/bybit    DIR_BYBIT_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/gateio    DIR_GATEIO_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/operation  DIR_OPERATION_SRCS)


include_directories(
        ${3RD_PATH}
        ${INC_DIR}
        ${SYSTEM_INC_DIR}
        ${FMT_INCLUDE_DIR}
)

link_directories(
        ${FMT_LIBRARY}
)

link_libraries(
        pthread
        boost_system 
        crypto 
        ssl 
        cpprest 
        z 
        rt  
        dl 
        cpp_redis 
        tacopie 
        tbb 
        stdc++fs 
        fmt
)

add_executable(ContractInfo
        ${DIRSRCS}
        ${DIRSRCS_BASE}
        ${DIR_BINANCE_SRCS}
        ${DIR_OKX_SRCS}
        ${DIR_BYBIT_SRCS}
        ${DIR_GATEIO_SRCS}
        ${DIR_OPERATION_SRCS}
        )

add_executable(GetInfo
        tools/GetInfo.cpp
        )
