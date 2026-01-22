cmake_minimum_required(VERSION 3.10)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(UEIDAQ-System LANGUAGES C CXX)

# =========================================================
# 1. 選項：SIM_BUILD=ON 時不依賴 UEI SDK，使用本機編譯器與模擬裝置
# =========================================================
option(SIM_BUILD "Build simulation-only binary without UEI hardware libs" ON)

if(NOT SIM_BUILD)
    # 交叉編譯設定（powerpc）
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR powerpc)
    set(UEI_SDK_ROOT "/opt/uei/ueipac-4.0.2")
    set(CMAKE_C_COMPILER "${UEI_SDK_ROOT}/powerpc-604-linux-gnu/bin/powerpc-604-linux-gnu-gcc")
    set(CMAKE_CXX_COMPILER "${UEI_SDK_ROOT}/powerpc-604-linux-gnu/bin/powerpc-604-linux-gnu-g++")
else()
    message(STATUS "SIM_BUILD=ON: building host simulation binary (no UEI SDK linkage).")
    set(CMAKE_C_COMPILER gcc)
    set(CMAKE_CXX_COMPILER g++)
endif()

# =========================================================
# 2. 路徑設定
# =========================================================
set(PROJECT_ROOT "${CMAKE_SOURCE_DIR}")
include_directories(
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/include/utils
    ${PROJECT_ROOT}/include/net
    ${PROJECT_ROOT}/include/nlohmann
    ${PROJECT_ROOT}/include/daq
    ${PROJECT_ROOT}/src
)

if(NOT SIM_BUILD)
    set(UEI_UTILS_DIR "${UEI_SDK_ROOT}/sdk/examples/UeiPacUtils")
    include_directories(
        ${UEI_SDK_ROOT}/include
        ${UEI_UTILS_DIR}
    )
    link_directories(${UEI_SDK_ROOT}/lib)
endif()

# =========================================================
# 3. 編譯目標
# =========================================================
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -g)

# 來源檔案列表
set(SOURCE_FILES
    main.cpp    
    src/utils/ConfigLoader.cpp
    src/daq/SimDaqDevice.cpp
    src/daq/DAQFactory.cpp
    src/net/UdpSender.cpp
    src/utils/CsvPacketizer.cpp
    src/dsp/Moving_Average.cpp
    src/dsp/FftProcessor.cpp
    src/dsp/kiss_fft.c
    src/dsp/kiss_fftr.c
)

if(NOT SIM_BUILD)
    list(APPEND SOURCE_FILES
        src/daq/DAQ_VMAP_AI217.cpp
        src/daq/DAQ_VMAP_AI211.cpp        
        "${UEI_UTILS_DIR}/UeiPacUtils.c"
    )
endif()

add_executable(ueipac_app ${SOURCE_FILES})

# 連結函式庫
target_link_libraries(ueipac_app pthread m)
if(NOT SIM_BUILD)
    target_link_libraries(ueipac_app powerdna)
endif()

if(SIM_BUILD)
    target_compile_definitions(ueipac_app PRIVATE SIM_BUILD=1)
endif()
