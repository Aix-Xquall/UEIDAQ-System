//=============================================================================
// NAME:    include/utils/UeiStructs.h
//=============================================================================
#pragma once

#include <string>
#include <vector>
#include <map>

namespace Utils
{

    // FFT 設定結構
    struct FftConfig
    {
        bool active;
        std::string windowType; // e.g., "Hann", "Blackman"
        int points;
        double overlapPercent;
    };

    // Moving Average 設定結構
    struct MovingAvgConfig
    {
        bool active;
        int windowSize;
    };

    // 硬體特定參數 (整合所有卡的特殊需求)
    struct HardwareConfig
    {
        // AI-208 Specific
        double excitationA = 0.0;
        double excitationB = 0.0;

        // AI-211 Specific
        std::string coupling = "DC"; // "AC", "DC"
        double iepeCurrent = 0.0;    // e.g., 0.004 for 4mA

        // General (Gain) - 適用於 217, 208, 211, 225
        int gain = 1;
    };

    // 通道設定結構
    struct ChannelConfig
    {
        std::string deviceName;   // 用於 UDP Header 識別
        std::string channelRange; // e.g., "ai0:3"
        std::string modelInfo;    // 描述資訊
        bool active;

        HardwareConfig hwConfig;   // 硬體參數
        MovingAvgConfig avgConfig; // 降頻/平滑參數
        FftConfig fftConfig;       // 頻譜分析參數
    };

    // 任務設定結構 (對應一個 I/O 卡/Slot)
    struct TaskConfig
    {
        std::string taskName;
        bool active;
        double sampleRate;
        std::vector<ChannelConfig> channels;
    };

    // 系統總設定
    struct SystemConfig
    {
        std::string systemName;
        std::string udpIp;
        int udpPort;
        std::vector<TaskConfig> taskConfigs;
    };

} // namespace Utils