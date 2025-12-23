//=============================================================================
// NAME:    src/daq/DaqAI217.cpp
// DESC:    AI-217 實作邏輯
//=============================================================================
#include "daq/DaqAI217.hpp" // 指向 include/daq/DaqAI217.hpp
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/time.h>

// 引入 SDK
#include "PDNA.h"
extern "C"
{
#include "UeiPacUtils.h"
}

namespace Daq
{

    DaqAI217::DaqAI217(const Utils::TaskConfig &config)
        : UeiDaqDevice(config)
    {
    }

    DaqAI217::~DaqAI217()
    {
        Stop();
        if (m_handle)
        {
            DqCloseIOM(m_handle);
            m_handle = 0;
        }
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
            std::cerr << "[AI217] Warning: Invalid gain " << gainVal << ", using 1." << std::endl;
            return DQ_AI217_GAIN_1;
        }
    }

    bool DaqAI217::Configure()
    {
        std::cout << "[AI217] Configuring Task: " << m_config.taskName << "..." << std::endl;

        // 1. 初始化 Lib
        DqInitDAQLib();

        // 2. 連接 IOM (本機)
        int ret = DqOpenIOM((char *)"127.0.0.1", DQ_UDP_DAQ_PORT, 2000, &m_handle, NULL);
        if (ret < 0)
        {
            std::cerr << "[AI217] Error: DqOpenIOM failed. Code: " << ret << std::endl;
            return false;
        }

        // 3. 設定通道列表 (Channel List)
        // 為了 Step 4 驗證，我們暫時簡化：直接使用 JSON 設定的通道數
        // 實際專案應實作完整的 Channel Parsing
        return true;
    }

    void DaqAI217::DaqLoop()
    {
        std::cout << "[AI217] Starting Acquisition Loop..." << std::endl;

        int ret;
        int device = 0; // 暫時假設 Slot 0 (需確認 JSON device_name 對應)

        // 準備通道清單
        uint32 clList[DQ_AI217_CHAN];
        int numCh = 8; // [驗證用] 強制讀取 ai0~ai7

        // 假設所有通道 Gain 一致
        int gainCode = GetGainCode(m_config.channels[0].hwConfig.gain);

        for (int i = 0; i < numCh; i++)
        {
            clList[i] = i | DQ_LNCL_GAIN(gainCode) | DQ_LNCL_DIFF;
        }

        // 設定 Clock
        uint32 clkEntries = 1;
        DQSETCLK clkSet;
        float actualClkRate;
        float reqRate = (float)m_config.sampleRate;

        clkSet.dev = device | DQ_LASTDEV;
        clkSet.ss = DQ_SS0IN;
        clkSet.clocksel = DQ_LN_CLKID_CVIN;
        memcpy((void *)&clkSet.frq, (void *)&reqRate, sizeof(clkSet.frq));

        ret = DqCmdSetClock(m_handle, &clkSet, &actualClkRate, &clkEntries);
        if (ret < 0)
        {
            std::cerr << "[AI217] SetClock Failed: " << ret << std::endl;
            m_running = false;
            return;
        }
        std::cout << "[AI217] Clock Set: " << actualClkRate << " Hz" << std::endl;

        // 資料緩衝區
        uint32 rawData[DQ_AI217_CHAN];
        double scaledData[DQ_AI217_CHAN];

        // 擷取迴圈
        while (m_running)
        {
            ret = DqAdv217Read(m_handle, device, numCh, clList, rawData, scaledData);

            if (ret < 0)
            {
                std::cerr << "[AI217] Read Error: " << ret << std::endl;
                usleep(100000);
                continue;
            }

            // 打包資料
            std::vector<double> frameData;
            frameData.reserve(numCh);
            for (int i = 0; i < numCh; i++)
            {
                frameData.push_back(scaledData[i]);
            }

            PushData(frameData);
        }

        std::cout << "[AI217] Loop Stopped." << std::endl;
    }

} // namespace Daq