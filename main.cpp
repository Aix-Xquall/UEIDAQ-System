#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "utils/ConfigLoader.hpp"
#include "utils/CsvPacketizer.hpp"
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
      throw std::runtime_error("No streaming devices created. Check slots[].active and channel_groups[].stream_active.");
    }

    // 3) Open UDP
    uei::UdpSender udp;
    udp.Open(settings.udp_target_ip, settings.udp_target_port);

    // MVP: run first device only (AI-217 one stream group)
    uei::DAQDevice &dev = *devices.front();
    dev.Open();
    dev.Start();

    while (true)
    {
      uei::RawFrame frame;
      if (!dev.ReadFrame(frame))
        break;

      const std::string payload = uei::CsvPacketizer::Encode(frame);
      udp.Send(payload.data(), payload.size());
    }

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
