//=============================================================================
// NAME:    include/daq/UeiDaqDevice.hpp
// DESC:    所有 DAQ 板卡的父類別 (已修正支援 Raw Data Batch)
//=============================================================================
#pragma once

#include "utils/UeiStructs.h"
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <cstdint> // for uint32_t
#include <sys/time.h>

namespace Daq
{

    // [修正] 定義內部傳遞的資料封包 (支援 Raw Batch)
    struct RawDataPacket
    {
        double timestamp;              // 第一筆資料的時間戳記
        std::vector<uint32_t> rawData; // [變更] 原始 ADC Code (uint32)
        int numSamples;                // [新增] 這個 Batch 包含多少個取樣點
    };

    class UeiDaqDevice
    {
    public:
        UeiDaqDevice(const Utils::TaskConfig &config)
            : m_config(config), m_running(false), m_handle(0) {}

        virtual ~UeiDaqDevice() { Stop(); }

        // --- 公用介面 ---

        virtual bool Configure() = 0;

        virtual void Start()
        {
            if (m_running)
                return;
            m_running = true;
            m_workerThread = std::thread(&UeiDaqDevice::DaqLoop, this);
        }

        virtual void Stop()
        {
            m_running = false;
            if (m_workerThread.joinable())
            {
                m_workerThread.join();
            }
        }

        bool PopData(RawDataPacket &packet)
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            if (m_dataQueue.empty())
                return false;
            packet = m_dataQueue.front();
            m_dataQueue.pop();
            return true;
        }

        const Utils::TaskConfig &GetConfig() const { return m_config; }

    protected:
        // --- 內部使用 ---

        // [修正] 這裡改為直接接受完整的 RawDataPacket
        void PushData(const RawDataPacket &packet)
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_dataQueue.push(packet);

            // 避免佇列無限膨脹 (例如保留最新 100 筆 Batch)
            if (m_dataQueue.size() > 100)
            {
                m_dataQueue.pop();
            }
        }

        virtual void DaqLoop() = 0;

        Utils::TaskConfig m_config;
        std::atomic<bool> m_running;
        int m_handle;
        std::thread m_workerThread;

        std::queue<RawDataPacket> m_dataQueue;
        std::mutex m_queueMutex;
    };

} // namespace Daq