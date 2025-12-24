/**
 * @file UdpSender.hpp
 * @brief UDP 發送模組，支援高效能二進位傳輸
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <netinet/in.h>

namespace Net
{
// 定義二進位封包結構 (Header + Payload)
// 讓 Python 端可以用 struct.unpack 解析
#pragma pack(push, 1) // 取消記憶體對齊，確保封包大小緊湊
    struct UdpHeader
    {
        uint32_t seqId;       // 封包序號
        double timestamp;     // 第一筆資料的時間戳
        uint16_t numSamples;  // 這個封包包含多少個 Sample
        uint16_t numChannels; // 通道數
    };
#pragma pack(pop)

    class UdpSender
    {
    public:
        UdpSender();
        ~UdpSender();

        /**
         * @brief 初始化 UDP Socket
         * @param targetIp 目標 IP
         * @param port 目標 Port
         * @return true 成功, false 失敗
         */
        bool Init(const std::string &targetIp, int port);

        /**
         * @brief 發送原始 ADC 數值 (Binary Batch)
         * @param seqId 序號
         * @param timestamp 時間戳
         * @param rawData 所有通道的原始數據 (interleaved: ch0, ch1, ch0, ch1...)
         * @param numSamples 樣本數 (Frames)
         * @param numChannels 通道數
         */
        void SendRawBatch(uint32_t seqId,
                          double timestamp,
                          const std::vector<uint32_t> &rawData,
                          uint16_t numSamples,
                          uint16_t numChannels);

        void Close();

    private:
        int m_sockfd;
        struct sockaddr_in m_servaddr;
        bool m_initialized;
    };
}