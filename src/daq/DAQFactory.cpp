#include "daq/DAQFactory.hpp"

#include <stdexcept>
#include <unordered_set>

#ifndef SIM_BUILD
#include "daq/DAQ_VMAP_AI217.hpp"
#endif
#include "daq/SimDaqDevice.hpp"

namespace uei
{

    static void Require(bool cond, const std::string &msg)
    {
        if (!cond)
            throw std::runtime_error("DAQFactory error: " + msg);
    }

    std::vector<std::unique_ptr<DAQDevice>> DAQFactory::CreateDevices(const Settings &settings)
    {
        std::vector<std::unique_ptr<DAQDevice>> devices;

        Require(settings.numSamplesPerChannel > 0, "numSamplesPerChannel must be > 0");

        const bool sim_active = settings.daq_simulation.active;

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

            std::vector<int> merged_channels;
            std::unordered_set<int> seen_channels;
            bool any_group = false;

            for (std::vector<ChannelGroupConfig>::const_iterator ig = slot.channel_groups.begin();
                 ig != slot.channel_groups.end(); ++ig)
            {
                const ChannelGroupConfig &g = *ig;
                if (!g.active)
                    continue;

                any_group = true;
                Require(!g.group_name.empty(), "AI-217: group_name must not be empty");
                Require(!g.channels.empty(), "AI-217: group channels must not be empty");

                for (size_t i = 0; i < g.channels.size(); ++i)
                {
                    const int ch = g.channels[i];
                    if (seen_channels.count(ch))
                        throw std::runtime_error("DAQFactory error: slot " + std::to_string(slot.slot_index) +
                                                 " channel overlap detected: ch=" + std::to_string(ch));
                    seen_channels.insert(ch);
                    merged_channels.push_back(ch);
                }
            }

            Require(any_group, "AI-217: no active channel_groups found");
            Require(!merged_channels.empty(), "AI-217: merged channel list is empty");

            const std::string group_name = "slot_" + std::to_string(slot.slot_index);

            if (sim_active)
            {
                SimDaqDevice::Params params;
                params.slot_index = slot.slot_index;
                params.group_name = group_name;
                params.channels = merged_channels;
                params.sample_rate_hz = slot.sample_rate_hz;
                params.samples_per_channel = settings.numSamplesPerChannel;
                params.base_frequency = settings.daq_simulation.base_frequency;
                params.frequency_step_percent = settings.daq_simulation.frequency_step_percent;
                params.amplitude = settings.daq_simulation.amplitude;
                params.noise_percent = settings.daq_simulation.noise_percent;
                devices.push_back(std::unique_ptr<DAQDevice>(new SimDaqDevice(params)));
            }
#ifndef SIM_BUILD
            else
            {
                DAQ_VMAP_AI217::PDNA_PARAMS params;

                params.device = slot.device_id;
                params.numChannels = static_cast<int>(merged_channels.size());
                params.channels = merged_channels;

                params.frequency = slot.sample_rate_hz;
                params.numSamplesPerChannel = settings.numSamplesPerChannel;

                params.slot_index = slot.slot_index;
                params.group_name = group_name;

                params.iom_ip = settings.uei.iom_ip;
                params.open_timeout_ms = settings.uei.open_timeout_ms;
                params.enable_rt = settings.uei.enable_rt;
                params.rt_priority = settings.uei.rt_priority;

                params.gain = slot.ai_config.gain;
                params.input_mode = slot.ai_config.input_mode;

                devices.push_back(std::unique_ptr<DAQDevice>(new DAQ_VMAP_AI217(params)));
            }
#else
            else
            {
                throw std::runtime_error("DAQFactory: hardware device requested but SIM_BUILD is ON");
            }
#endif
        }

        return devices;
    }

} // namespace uei
