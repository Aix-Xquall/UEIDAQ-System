cmake_minimum_required(VERSION 3.10)

# =========================================================
# 1. 系統與編譯器設定
# =========================================================
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR powerpc)
set(UEI_SDK_ROOT "/opt/uei/ueipac-4.0.2")

set(CMAKE_C_COMPILER "${UEI_SDK_ROOT}/powerpc-604-linux-gnu/bin/powerpc-604-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${UEI_SDK_ROOT}/powerpc-604-linux-gnu/bin/powerpc-604-linux-gnu-g++")

project(UEIDAQ-System LANGUAGES C CXX)

# =========================================================
# 2. 路徑設定
# =========================================================
set(UEI_UTILS_DIR "${UEI_SDK_ROOT}/sdk/examples/UeiPacUtils")
set(PROJECT_ROOT "${CMAKE_SOURCE_DIR}")

# [關鍵] 加入專案內部的 include 資料夾
include_directories(
    ${UEI_SDK_ROOT}/include
    ${UEI_UTILS_DIR}    
    ${PROJECT_ROOT}/include        # 讓 #include "daq/DaqAI217.hpp" 能找到檔案
    ${PROJECT_ROOT}/include/utils  # 選擇性加入
    ${PROJECT_ROOT}/include/net
    ${PROJECT_ROOT}/src            # 為了相容 ConfigLoader.cpp 
)

# 加入函式庫路徑
link_directories(${UEI_SDK_ROOT}/lib)


# =========================================================
# 3. 編譯目標
# =========================================================
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -g) # 只保留通用選項


# 來源檔案列表 (注意 ConfigLoader 路徑，若您也移動了它請更新)
set(SOURCE_FILES
    main.cpp    
    src/utils/ConfigLoader.cpp
    src/daq/DaqAI217.cpp
    src/net/UdpSender.cpp
    "${UEI_UTILS_DIR}/UeiPacUtils.c"
)

add_executable(ueipac_app ${SOURCE_FILES})

# 連結函式庫
target_link_libraries(ueipac_app powerdna pthread m)