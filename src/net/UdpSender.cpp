/**
 * @file UdpSender.cpp
 * @brief UDP 發送實作
 */
#include "net/UdpSender.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <vector>

namespace Net
{

    UdpSender::UdpSender() : m_sockfd(-1), m_initialized(false) {}

    UdpSender::~UdpSender() { Close(); }

    bool UdpSender::Init(const std::string &targetIp, int port)
    {
        if (m_initialized)
            Close();

        if ((m_sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        {
            std::cerr << "[UDP] Socket creation failed" << std::endl;
            return false;
        }

        memset(&m_servaddr, 0, sizeof(m_servaddr));
        m_servaddr.sin_family = AF_INET;
        m_servaddr.sin_port = htons(port);

        if (inet_aton(targetIp.c_str(), &m_servaddr.sin_addr) == 0)
        {
            std::cerr << "[UDP] Invalid IP: " << targetIp << std::endl;
            return false;
        }

        m_initialized = true;
        std::cout << "[UDP] Initialized Target: " << targetIp << ":" << port << std::endl;
        return true;
    }

    void UdpSender::SendRawBatch(uint32_t seqId,
                                 double timestamp,
                                 const std::vector<uint32_t> &rawData,
                                 uint16_t numSamples,
                                 uint16_t numChannels)
    {
        if (!m_initialized)
            return;

        // 準備 Buffer: Header + Data
        std::vector<uint8_t> buffer;
        size_t payloadSize = rawData.size() * sizeof(uint32_t);
        buffer.resize(sizeof(UdpHeader) + payloadSize);

        // 填寫 Header
        UdpHeader *header = reinterpret_cast<UdpHeader *>(buffer.data());
        header->seqId = seqId;
        header->timestamp = timestamp;
        header->numSamples = numSamples;
        header->numChannels = numChannels;

        // 填寫 Data (直接記憶體複製)
        std::memcpy(buffer.data() + sizeof(UdpHeader), rawData.data(), payloadSize);

        // 發送
        sendto(m_sockfd, buffer.data(), buffer.size(), 0,
               (const struct sockaddr *)&m_servaddr, sizeof(m_servaddr));
    }

    void UdpSender::Close()
    {
        if (m_sockfd >= 0)
        {
            close(m_sockfd);
            m_sockfd = -1;
        }
        m_initialized = false;
    }
}