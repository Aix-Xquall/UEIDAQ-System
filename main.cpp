/**
 * @file main.cpp
 * @brief 主程式：接收 Batch Data 並透過 Binary UDP 發送
 */
#include <iostream>
#include <unistd.h>
#include <signal.h>
#include "utils/ConfigLoader.hpp"
#include "daq/DaqAI217.hpp"
#include "net/UdpSender.hpp"

volatile sig_atomic_t g_stop = 0;
void signal_handler(int) { g_stop = 1; }

int main()
{
    signal(SIGINT, signal_handler);

    // ... Config Loading 代碼省略 ...
    auto sysConfig = Utils::ConfigLoader::load("DAQ_Settings.json");
    Net::UdpSender udpSender;
    udpSender.Init(sysConfig.udpIp, sysConfig.udpPort);

    // ... Daq 初始化代碼省略 ...
    Utils::TaskConfig *ai217Config = &sysConfig.taskConfigs[0]; // 簡化範例
    Daq::DaqAI217 ai217Device(*ai217Config);
    ai217Device.Configure();
    ai217Device.Start();

    Daq::RawDataPacket packet;
    long seqId = 0;
    int numCh = 8; // 假設 8 通道

    while (!g_stop)
    {
        // 從 Queue 取出一個 Batch (包含 10 個 Samples)
        if (ai217Device.PopData(packet))
        {
            seqId++;
            // 發送二進位封包
            udpSender.SendRawBatch(seqId,
                                   packet.timestamp,
                                   packet.rawData,
                                   packet.numSamples,
                                   numCh);
        }
        else
        {
            usleep(1000); // 稍微休息，釋放 CPU
        }
    }

    ai217Device.Stop();
    udpSender.Close();
    return 0;
}