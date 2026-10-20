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
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/bitunix   DIR_BITUNIX_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/phemex    DIR_PHEMEX_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/weex      DIR_WEEX_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/fameex    DIR_FAMEEX_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/blofin    DIR_BLOFIN_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/hotcoin   DIR_HOTCOIN_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/echobit   DIR_ECHOBIT_SRCS)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src/mgbx      DIR_MGBX_SRCS)
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

# ============================================================================
# 核心库：全部交易所解析器 + 操作层。
#
# 为什么抽成 OBJECT 库:
#   工具（InfoCheck）要跑**真实**的 XxxInfo::syncExchangeInfo()，而不是另抄一份
#   解析逻辑 —— 抄的那份必然和 C++ 漂移，于是"新交易所验证"变成"验证我抄得对不对"。
#   所以把解析器编成一份对象库，主程序和工具都用 $<TARGET_OBJECTS:> 复用，
#   不重复编译、不重复链接。
#
# src/main.cpp 自带 main()，不能进这个库（工具各有自己的 main）。
# ============================================================================
list(REMOVE_ITEM DIRSRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

# src/ 下目前只有 main.cpp。如果以后有人往 src/ 直接丢 .cpp，它会既不在核心库里
# 也不在可执行文件里 —— 表现为一堆 undefined reference，很难查。这里直接拦下来。
if (DIRSRCS)
    message(FATAL_ERROR
        "src/ 下还有未归属的源文件: ${DIRSRCS}\n"
        "请把它们加进 ContractInfoCore 的源列表，或挪到 src/<exchange>/ 下。")
endif()

add_library(ContractInfoCore OBJECT
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
        ${DIR_BITUNIX_SRCS}
        ${DIR_PHEMEX_SRCS}
        ${DIR_WEEX_SRCS}
        ${DIR_FAMEEX_SRCS}
        ${DIR_BLOFIN_SRCS}
        ${DIR_HOTCOIN_SRCS}
        ${DIR_ECHOBIT_SRCS}
        ${DIR_MGBX_SRCS}
        ${DIR_OPERATION_SRCS}
        )

add_executable(ContractInfo
        ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
        $<TARGET_OBJECTS:ContractInfoCore>
        )

# ---------------------------------------------------------------------------
# 工具
#   GetInfo     读侧：SecurityManager 查 SHM/Redis，看 db 实际能拿到什么（纯头文件，不链核心库）
#   InfoCheck   写侧：真实解析器拉一遍接口 + 32 项不变量检查（链核心库）
#   DumpInfoShm 裸 SHM dump
#   TestInfoShm SHM 读写正确性
#   BenchInfoShm SHM 性能
# ---------------------------------------------------------------------------
add_executable(GetInfo tools/GetInfo.cpp)

add_executable(InfoCheck
        tools/InfoCheck.cpp
        $<TARGET_OBJECTS:ContractInfoCore>
        )

add_executable(DumpInfoShm tools/DumpInfoShm.cpp)
add_executable(TestInfoShm tools/TestInfoShm.cpp)
add_executable(BenchInfoShm tools/BenchInfoShm.cpp)

# 规则自测：只依赖 tools/InfoCheckRules.h（纯 std），**不需要** boost/simdjson/
# rapidjson/fmt，所以本机（macOS）也能编能跑。改完任何一条检查项先跑它：
#   ./bin/selftest_rules
add_executable(selftest_rules tools/selftest_rules.cpp)
