//=============================================================================
// NAME:    src/utils/ConfigLoader.cpp
//=============================================================================
#include "utils/ConfigLoader.hpp"
#include "nlohmann/json.hpp" // 請確保此檔案已存在
#include <fstream>
#include <stdexcept>
#include <iostream>

// 為了方便使用 JSON 物件
using json = nlohmann::json;

namespace Utils
{

    SystemConfig ConfigLoader::load(const std::string &filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            // 嘗試從上層目錄尋找 (相容 build 資料夾執行情況)
            file.open("../" + filePath);
            if (!file.is_open())
            {
                throw std::runtime_error("[Config] Cannot open config file: " + filePath);
            }
        }

        try
        {
            json j;
            file >> j;

            SystemConfig sysConfig;
            sysConfig.systemName = j.value("system_name", "DefaultSystem");
            sysConfig.udpIp = j.value("udp_target_ip", "127.0.0.1");
            sysConfig.udpPort = j.value("udp_target_port", 5005);

            // 解析 Tasks
            if (j.contains("tasks"))
            {
                for (const auto &taskJson : j["tasks"])
                {
                    TaskConfig task;
                    task.taskName = taskJson.value("task_name", "UnnamedTask");
                    task.active = taskJson.value("active", false);
                    task.sampleRate = taskJson.value("sample_rate", 1000.0);

                    if (!task.active)
                        continue; // 跳過未啟用任務

                    // 解析 Channels
                    if (taskJson.contains("channels"))
                    {
                        for (const auto &chJson : taskJson["channels"])
                        {
                            ChannelConfig ch;
                            ch.deviceName = chJson.value("device_name", "UnknownDev");
                            ch.channelRange = chJson.value("channel_range", "ai0");
                            ch.modelInfo = chJson.value("model_info", "");
                            ch.active = chJson.value("active", true);

                            if (!ch.active)
                                continue; // 跳過未啟用通道

                            // 1. Moving Average Config
                            if (chJson.contains("moving_avg"))
                            {
                                ch.avgConfig.active = chJson["moving_avg"].value("active", false);
                                ch.avgConfig.windowSize = chJson["moving_avg"].value("window_size", 1);
                            }

                            // 2. FFT Config
                            if (chJson.contains("fft"))
                            {
                                ch.fftConfig.active = chJson["fft"].value("active", false);
                                ch.fftConfig.windowType = chJson["fft"].value("window_type", "Hann");
                                ch.fftConfig.points = chJson["fft"].value("points", 1024);
                                ch.fftConfig.overlapPercent = chJson["fft"].value("overlap_percent", 0.0);
                            }

                            // 3. Hardware Config (關鍵新增部分)
                            if (chJson.contains("hardware_config"))
                            {
                                auto hw = chJson["hardware_config"];
                                // AI-208
                                ch.hwConfig.excitationA = hw.value("ai208_excitation_a", 0.0);
                                ch.hwConfig.excitationB = hw.value("ai208_excitation_b", 0.0);
                                // AI-211
                                ch.hwConfig.coupling = hw.value("ai211_coupling", "DC");
                                ch.hwConfig.iepeCurrent = hw.value("ai211_iepe_current", 0.0);
                                // General
                                ch.hwConfig.gain = hw.value("gain", 1);
                                if (hw.contains("ai217_gain"))
                                    ch.hwConfig.gain = hw["ai217_gain"];
                                if (hw.contains("ai208_gain"))
                                    ch.hwConfig.gain = hw["ai208_gain"];
                                if (hw.contains("ai211_gain"))
                                    ch.hwConfig.gain = hw["ai211_gain"];
                            }

                            task.channels.push_back(ch);
                        }
                    }

                    // 只有當 Task 內有有效 Channel 時才加入
                    if (!task.channels.empty())
                    {
                        sysConfig.taskConfigs.push_back(task);
                    }
                }
            }

            std::cout << "[Config] Successfully loaded: " << sysConfig.systemName
                      << " (" << sysConfig.taskConfigs.size() << " active tasks)" << std::endl;

            return sysConfig;
        }
        catch (const json::exception &e)
        {
            throw std::runtime_error("[Config] JSON Parse Error: " + std::string(e.what()));
        }
    }

} // namespace Utils