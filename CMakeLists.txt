cmake_minimum_required(VERSION 3.1)
project(ContractInfo)

# 设置编译器 c++17 required
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE "Debug")
#set(CMAKE_BUILD_TYPE "Release")
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -fPIC -Wall -g -ggdb")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -fPIC -DABI -DNDEBUG -DOPENBLAS  -O3 -Wall -flto -fuse-linker-plugin -minline-all-stringops -malign-double -ffast-math -march=native -ffunction-sections -fdata-sections -fmessage-length=0 -msse -msse2 -msse3 -funroll-loops")

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/bin)

add_definitions(-DUSE_INFO_SHM)

#include
set(INC_DIR              ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(SYSTEM_INC_DIR          ../include)


aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src       DIRSRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/base     DIRSRCS_BASE)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/binance   DIR_BINANCE_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/okx     DIR_OKX_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/bybit    DIR_BYBIT_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/gateio    DIR_GATEIO_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/htx       DIR_HTX_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/bitget    DIR_BITGET_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/mexc      DIR_MEXC_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/kucoin    DIR_KUCOIN_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/bingx     DIR_BINGX_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/xt        DIR_XT_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/operation  DIR_OPERATION_SRCS)


include_directories(
        ${INC_DIR}
        ${SYSTEM_INC_DIR}
)

link_libraries(
        pthread
        boost_system 
        crypto 
        ssl 
        z 
        rt  
        dl 
        cpp_redis 
        tacopie 
        tbb 
        stdc++fs 
        fmt
        simdjson
)

add_executable(ContractInfo
        ${DIRSRCS}
        ${DIRSRCS_BASE}
        ${DIR_BINANCE_SRCS}
        ${DIR_OKX_SRCS}
        ${DIR_BYBIT_SRCS}
        ${DIR_GATEIO_SRCS}
        ${DIR_HTX_SRCS}
        ${DIR_BITGET_SRCS}
        ${DIR_MEXC_SRCS}
        ${DIR_KUCOIN_SRCS}
        ${DIR_BINGX_SRCS}
        ${DIR_XT_SRCS}
        ${DIR_OPERATION_SRCS}
        )

add_executable(GetInfo
        tools/GetInfo.cpp
        )

add_executable(DumpInfoShm tools/DumpInfoShm.cpp)
add_executable(TestInfoShm tools/TestInfoShm.cpp)
add_executable(BenchInfoShm tools/BenchInfoShm.cpp)
