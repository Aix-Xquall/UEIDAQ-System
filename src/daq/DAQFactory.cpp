#include "daq/DAQFactory.hpp"

#include <cmath>
#include <stdexcept>

#include "daq/DAQ_VMAP_AI217.hpp"

namespace uei
{

    static void Require(bool cond, const std::string &msg)
    {
        if (!cond)
            throw std::runtime_error("DAQFactory error: " + msg);
    }

    static int DeriveSamplesPerChannel(double sample_rate_hz, int packet_interval_ms)
    {
        const double s = sample_rate_hz * (static_cast<double>(packet_interval_ms) / 1000.0);
        int v = static_cast<int>(std::lround(s));
        if (v < 1)
            v = 1;
        return v;
    }

    std::vector<std::unique_ptr<DAQDevice>> DAQFactory::CreateDevices(const Settings &settings)
    {
        std::vector<std::unique_ptr<DAQDevice>> devices;

        for (std::vector<SlotConfig>::const_iterator it = settings.slots.begin();
             it != settings.slots.end(); ++it)
        {
            const SlotConfig &slot = *it;
            if (!slot.active)
                continue;

            // MVP: only AI-217
            if (slot.board_name != "DNA-AI-217")
                continue;

            Require(slot.sample_rate_hz > 0.0, "AI-217: sample_rate must be > 0");
            Require(!slot.channel_groups.empty(), "AI-217: channel_groups[] must not be empty");

            for (std::vector<ChannelGroupConfig>::const_iterator ig = slot.channel_groups.begin();
                 ig != slot.channel_groups.end(); ++ig)
            {
                const ChannelGroupConfig &g = *ig;

                // Rule A: group.active=true means stream it
                if (!g.active)
                    continue;

                Require(!g.group_name.empty(), "AI-217: group_name must not be empty");
                Require(!g.channels.empty(), "AI-217: group channels must not be empty");

                const int samples_per_channel = DeriveSamplesPerChannel(slot.sample_rate_hz, settings.packet_interval_ms);

                DAQ_VMAP_AI217::Params p;
                p.iom_ip = settings.uei.iom_ip;
                p.open_timeout_ms = settings.uei.open_timeout_ms;
                p.enable_rt = settings.uei.enable_rt;
                p.rt_priority = settings.uei.rt_priority;

                p.device_id = slot.device_id;
                p.slot_index = slot.slot_index;
                p.group_name = g.group_name;

                p.subsystem = slot.subsystem;
                p.scan_rate_hz = slot.sample_rate_hz;
                p.samples_per_channel = samples_per_channel;
                p.channels = g.channels;

                p.gain = slot.ai_config.gain;
                p.input_mode = slot.ai_config.input_mode;

                p.packet_interval_ms = settings.packet_interval_ms;

                devices.push_back(std::unique_ptr<DAQDevice>(new DAQ_VMAP_AI217(p)));
            }
        }

        return devices;
    }

} // namespace uei
