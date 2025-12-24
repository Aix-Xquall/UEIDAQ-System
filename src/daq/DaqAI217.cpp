/**
 * @file DaqAI217.cpp
 * @brief AI-217 實作 (修正：加入 Loop Pacing 防止重複讀取)
 */
#include "daq/DaqAI217.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <unistd.h>   // for usleep
#include <sys/time.h> // for gettimeofday
#include "PDNA.h"
extern "C"
{
#include "UeiPacUtils.h"
}

namespace Daq
{
    // 設定 Batch 大小
    // 100Hz 取樣下，設定 10 代表每 0.1秒送一次 UDP 封包
    static const int BATCH_SIZE = 10;

    DaqAI217::DaqAI217(const Utils::TaskConfig &config) : UeiDaqDevice(config) {}

    DaqAI217::~DaqAI217()
    {
        Stop();
        if (m_handle)
            DqCloseIOM(m_handle);
    }

    int DaqAI217::GetGainCode(int gainVal)
    {
        switch (gainVal)
        {
        case 1:
            return DQ_AI217_GAIN_1;
        case 2:
            return DQ_AI217_GAIN_2;
        case 4:
            return DQ_AI217_GAIN_4;
        case 8:
            return DQ_AI217_GAIN_8;
        default:
            return DQ_AI217_GAIN_1;
        }
    }

    bool DaqAI217::Configure()
    {
        DqInitDAQLib();
        int ret = DqOpenIOM((char *)"127.0.0.1", DQ_UDP_DAQ_PORT, 2000, &m_handle, NULL);
        if (ret < 0)
        {
            std::cerr << "[AI217] OpenIOM Failed: " << ret << std::endl;
            return false;
        }
        return true;
    }
    void DaqAI217::DaqLoop()
    {
        std::cout << "[AI217] Configuring Clock..." << std::endl;

        int device = 0;
        int numCh = 8;
        // ... (Channel List 設定同前) ...
        uint32 clList[DQ_AI217_CHAN];
        int gainCode = GetGainCode(m_config.channels[0].hwConfig.gain);
        for (int i = 0; i < numCh; i++)
            clList[i] = i | DQ_LNCL_GAIN(gainCode) | DQ_LNCL_DIFF;

        // --- 設定 Clock ---
        DQSETCLK clkSet;
        float actualClkRate; // [關鍵] 用這個變數來儲存硬體給的真實頻率
        float reqRate = (float)m_config.sampleRate;
        uint32 clkEntries = 1;

        clkSet.dev = device | DQ_LASTDEV;
        clkSet.ss = DQ_SS0IN;
        clkSet.clocksel = DQ_LN_CLKID_CVIN;
        memcpy((void *)&clkSet.frq, (void *)&reqRate, sizeof(clkSet.frq));

        if (DqCmdSetClock(m_handle, &clkSet, &actualClkRate, &clkEntries) < 0)
        {
            std::cerr << "[AI217] SetClock Failed" << std::endl;
            return;
        }

        std::cout << "[AI217] Requested: " << reqRate << " Hz, Actual: " << actualClkRate << " Hz" << std::endl;

        // [關鍵修正] 根據真實頻率計算休眠時間 (微秒)
        // 避免除以 0
        if (actualClkRate < 0.1)
            actualClkRate = 1.0;
        long period_us = (long)(1000000.0 / actualClkRate);

        // 初始化
        uint32 rawDataOneSample[DQ_AI217_CHAN];
        double scaledDummy[DQ_AI217_CHAN];
        std::vector<uint32_t> batchBuffer;
        batchBuffer.reserve(numCh * BATCH_SIZE);
        double batchStartTime = 0.0;
        int samplesCollected = 0;

        std::cout << "[AI217] Loop Starting with Period: " << period_us << " us" << std::endl;

        while (m_running)
        {
            struct timeval t1, t2;
            gettimeofday(&t1, NULL); // Loop Start

            // 讀取數據
            int ret = DqAdv217Read(m_handle, device, numCh, clList, rawDataOneSample, scaledDummy);

            if (ret >= 0)
            {
                if (samplesCollected == 0)
                {
                    batchStartTime = t1.tv_sec + t1.tv_usec / 1000000.0;
                }

                for (int i = 0; i < numCh; i++)
                {
                    batchBuffer.push_back(rawDataOneSample[i]);
                }
                samplesCollected++;

                if (samplesCollected >= BATCH_SIZE)
                {
                    RawDataPacket packet;
                    packet.timestamp = batchStartTime;
                    packet.numSamples = samplesCollected;
                    packet.rawData = batchBuffer;
                    PushData(packet);
                    batchBuffer.clear();
                    samplesCollected = 0;
                }
            }

            // [關鍵] 精確的軟體定時
            gettimeofday(&t2, NULL); // Loop End
            long elapsed_us = (t2.tv_sec - t1.tv_sec) * 1000000 + (t2.tv_usec - t1.tv_usec);

            // 只有當處理時間小於週期時才休眠
            long sleep_us = period_us - elapsed_us;

            // 補償機制：如果稍微 delay 了，這次就少睡一點 (簡單的 P 補償)
            // 這裡使用最基本的休眠，確保不佔用 100% CPU
            if (sleep_us > 100)
            {
                usleep(sleep_us);
            }
        }
    }
}