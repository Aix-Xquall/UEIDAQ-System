//=============================================================================
// NAME:    src/net/UdpSender.cpp
//=============================================================================
#include "net/UdpSender.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <iomanip> // for std::setprecision

namespace Net
{

    UdpSender::UdpSender() : m_sockfd(-1), m_initialized(false) {}

    UdpSender::~UdpSender()
    {
        Close();
    }

    bool UdpSender::Init(const std::string &targetIp, int port)
    {
        if (m_initialized)
            Close();

        // 建立 UDP Socket
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
            std::cerr << "[UDP] Invalid IP address: " << targetIp << std::endl;
            return false;
        }

        m_initialized = true;
        std::cout << "[UDP] Initialized Target: " << targetIp << ":" << port << std::endl;
        return true;
    }

    void UdpSender::SendPacket(const std::string &deviceName,
                               long seqId,
                               double timestamp,
                               const std::vector<double> &data)
    {
        if (!m_initialized)
            return;

        // 拼湊 CSV 字串 (效率優化: 使用 stringstream 或 sprintf)
        // Python 解析: parts = msg.split(',')
        // Header: DeviceName, SeqID, Timestamp, NumCh, Reserved
        std::stringstream ss;
        ss << std::fixed << std::setprecision(6);

        ss << deviceName << ","
           << seqId << ","
           << timestamp << ","
           << data.size() << ","
           << "0"; // 保留欄位

        // Data: Val0, Val1, ...
        for (double val : data)
        {
            ss << "," << val;
        }

        std::string payload = ss.str();

        // 發送
        sendto(m_sockfd, payload.c_str(), payload.length(), 0,
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

} // namespace Net