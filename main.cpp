#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>

#include "utils/ConfigLoader.hpp"
#include "utils/CsvPacketizer.hpp"
#include "utils/RingBuffer.hpp"
#include "net/UdpSender.hpp"
#include "daq/DAQFactory.hpp"

int main()
{
  try
  {
    // 1) Load default config (same directory)
    const uei::Settings settings = uei::ConfigLoader::LoadDefault();

    // 2) Create DAQ devices from JSON (MVP: AI-217 only)
    std::vector<std::unique_ptr<uei::DAQDevice>> devices = uei::DAQFactory::CreateDevices(settings);
    if (devices.empty())
    {
      throw std::runtime_error("No streaming devices created. Check slots[].active and channel_groups[].active.");
    }

    // MVP: run first device only (AI-217 one stream group)
    uei::DAQDevice &dev = *devices.front();

    // 3) Open UDP
    uei::UdpSender udp;
    udp.Open(settings.udp_target_ip, settings.udp_target_port);

    // 4) Open/Start DAQ
    dev.Open();
    dev.Start();

    // 5) RingBuffer between DAQ and UDP
    //    Capacity is "frames" count, not bytes. Increase if UDP side is slower.
    constexpr std::size_t kBufferFrames = 128;
    uei::RingBuffer<uei::RawFrame> rb(kBufferFrames);

    std::atomic<bool> stop{false};

    // --- Producer: DAQ thread (stable, never blocked by UDP) ---
    std::thread th_daq([&]()
                       {
      while (!stop.load())
      {
        uei::RawFrame frame;
        if (!dev.ReadFrame(frame))
        {
          stop.store(true);
          rb.Stop();
          break;
        }

        // If driver returns empty frame (0 scans), skip pushing to buffer.
        if (frame.samples_per_channel <= 0 || frame.num_channels <= 0 || frame.raw.empty())
        {
          continue;
        }

        rb.Push(std::move(frame));
      } });

    // --- Consumer: UDP sender thread ---
    std::thread th_udp([&]()
                       {
      while (!stop.load())
      {
        uei::RawFrame frame;
        if (!rb.PopFor(frame, std::chrono::milliseconds(200)))
        {
          // timeout: loop again to check stop flag
          continue;
        }

        //const std::string payload = uei::CsvPacketizer::Encode(frame);
        //udp.Send(payload.data(), payload.size());
        udp.Send("tesy",4);
      } });

    // 6) Wait until DAQ thread ends (CTRL+C is handled inside DAQ_VMAP_AI217.cpp with SIGINT -> stop)
    th_daq.join();

    // Signal consumer to stop and join
    stop.store(true);
    rb.Stop();
    th_udp.join();

    // 7) Clean up
    dev.Stop();
    dev.Close();
    udp.Close();

    return 0;
  }
  catch (const std::exception &e)
  {
    std::cerr << "[FATAL] " << e.what() << "\n";
    return 1;
  }
}
