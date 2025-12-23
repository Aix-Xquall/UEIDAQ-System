//=============================================================================
// NAME:    main.cpp (Step 5: UDP Integration)
//=============================================================================
#include <iostream>
#include <unistd.h>
#include <signal.h>
#include "utils/ConfigLoader.hpp"
#include "daq/DaqAI217.hpp"
#include "net/UdpSender.hpp" // [新增]

volatile sig_atomic_t g_stop = 0;

void signal_handler(int)
{
    g_stop = 1;
}

int main()
{
    signal(SIGINT, signal_handler);
    std::cout << "=== UEIPAC System Step 5 (UDP Live) ===" << std::endl;

    try
    {
        auto sysConfig = Utils::ConfigLoader::load("DAQ_Settings.json");

        // 1. 初始化 UDP Sender
        Net::UdpSender udpSender;
        if (!udpSender.Init(sysConfig.udpIp, sysConfig.udpPort))
        {
            std::cerr << "UDP Init Failed!" << std::endl;
            return -1;
        }

        // 2. 設定 AI-217
        Utils::TaskConfig *ai217Config = nullptr;
        for (auto &task : sysConfig.taskConfigs)
        {
            if (task.taskName == "Task_Slot0_AI217")
            {
                ai217Config = &task;
                break;
            }
        }

        if (!ai217Config)
        {
            std::cerr << "Config not found!" << std::endl;
            return -1;
        }

        Daq::DaqAI217 ai217Device(*ai217Config);
        if (!ai217Device.Configure())
            return -1;

        ai217Device.Start();

        // 3. 主迴圈：接收資料 -> UDP 轉發
        std::cout << "[Main] Sending data to " << sysConfig.udpIp << ":" << sysConfig.udpPort << std::endl;

        Daq::RawDataPacket packet;
        long packetCount = 0;

        // 取得 Device Name 用於封包 Header (從第一通道設定取)
        std::string devName = ai217Config->channels[0].deviceName;

        while (!g_stop)
        {
            if (ai217Device.PopData(packet))
            {
                packetCount++;

                // [關鍵] 透過 UDP 發送
                udpSender.SendPacket(devName, packetCount, packet.timestamp, packet.data);

                // Console 僅顯示狀態 (每 100 筆)
                if (packetCount % 100 == 0)
                {
                    std::cout << "\r[UDP] Sent: " << packetCount << " pkts" << std::flush;
                }
            }
            else
            {
                usleep(100); // UDP 模式下可以縮短休眠以提高反應
            }
        }

        ai217Device.Stop();
        udpSender.Close();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
        return -1;
    }
    return 0;
}