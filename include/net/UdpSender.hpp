//=============================================================================
// NAME:    include/net/UdpSender.hpp
// DESC:    負責將數據打包並透過 UDP 發送
//=============================================================================
#pragma once

#include <string>
#include <vector>
#include <netinet/in.h>

namespace Net
{

    class UdpSender
    {
    public:
        UdpSender();
        ~UdpSender();

        // 初始化網路 Socket
        bool Init(const std::string &targetIp, int port);

        // 發送 CSV 格式資料 (符合 Python udp_plotter 解析規則)
        // 格式: DeviceName,SeqID,Timestamp,NumCh,0,Val0,Val1...
        void SendPacket(const std::string &deviceName,
                        long seqId,
                        double timestamp,
                        const std::vector<double> &data);

        void Close();

    private:
        int m_sockfd;
        struct sockaddr_in m_servaddr;
        bool m_initialized;
    };

} // namespace Net