//=============================================================================
// NAME:    include/daq/UeiDaqDevice.hpp
// DESC:    所有 DAQ 板卡的父類別 (Abstract Base Class)
//=============================================================================
#pragma once

#include "utils/UeiStructs.h"
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <sys/time.h> // for gettimeofday

namespace Daq
{

    // 定義內部傳遞的資料封包 (Raw Data Packet)
    struct RawDataPacket
    {
        double timestamp;         // 資料時間戳記
        std::vector<double> data; // 各通道電壓值 (Scaled Data)
    };

    class UeiDaqDevice
    {
    public:
        UeiDaqDevice(const Utils::TaskConfig &config)
            : m_config(config), m_running(false), m_handle(0) {}

        virtual ~UeiDaqDevice() { Stop(); }

        // --- 公用介面 ---

        // 1. 初始化與硬體設定 (純虛擬函式，由子類別實作)
        virtual bool Configure() = 0;

        // 2. 啟動擷取 (建立執行緒)
        virtual void Start()
        {
            if (m_running)
                return;
            m_running = true;
            m_workerThread = std::thread(&UeiDaqDevice::DaqLoop, this);
        }

        // 3. 停止擷取
        virtual void Stop()
        {
            m_running = false;
            if (m_workerThread.joinable())
            {
                m_workerThread.join();
            }
        }

        // 4. 取得資料 (Thread-safe Pop)
        bool PopData(RawDataPacket &packet)
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            if (m_dataQueue.empty())
                return false;
            packet = m_dataQueue.front();
            m_dataQueue.pop();
            return true;
        }

        // 取得設定檔
        const Utils::TaskConfig &GetConfig() const { return m_config; }

    protected:
        // --- 內部使用 ---

        // 推送資料到佇列
        void PushData(const std::vector<double> &channelData)
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            // 簡單的時間戳記 (系統時間)
            struct timeval tv;
            gettimeofday(&tv, NULL);
            double ts = tv.tv_sec + tv.tv_usec / 1000000.0;

            m_dataQueue.push({ts, channelData});

            // 避免佇列無限膨脹 (例如保留最新 1000 筆)
            if (m_dataQueue.size() > 1000)
            {
                m_dataQueue.pop();
            }
        }

        // 實際的擷取迴圈 (由子類別實作細節)
        virtual void DaqLoop() = 0;

        // 成員變數
        Utils::TaskConfig m_config;
        std::atomic<bool> m_running;
        int m_handle; // PowerDNA Handle (hd0)
        std::thread m_workerThread;

        // 資料佇列
        std::queue<RawDataPacket> m_dataQueue;
        std::mutex m_queueMutex;
    };

} // namespace Daq