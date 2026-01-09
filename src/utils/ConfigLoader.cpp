#include "utils/ConfigLoader.hpp"

#include <fstream>
#include <stdexcept>

#include "nlohmann/json.hpp"

namespace uei
{

  static void Require(bool cond, const std::string &msg)
  {
    if (!cond)
      throw std::runtime_error("Config error: " + msg);
  }

  /** @brief sample_rate supports number OR {active,hz} (backward compatible). */
  static double ParseSampleRateHz(const nlohmann::json &js_slot)
  {
    if (!js_slot.contains("sample_rate"))
      return 0.0;
    const auto &sr = js_slot["sample_rate"];

    if (sr.is_number())
      return sr.get<double>();

    if (sr.is_object())
    {
      const bool active = sr.value("active", false);
      const double hz = sr.value("hz", 0.0);
      return active ? hz : 0.0;
    }

    return 0.0;
  }

  Settings ConfigLoader::LoadFromFile(const std::string &path)
  {
    std::ifstream ifs(path);
    if (!ifs)
      throw std::runtime_error("Failed to open config file: " + path);

    nlohmann::json j;
    ifs >> j;

    Settings s;
    s.system_name = j.value("system_name", "");
    s.udp_target_ip = j.value("udp_target_ip", "");
    s.udp_target_port = static_cast<uint16_t>(j.value("udp_target_port", 0));
    s.config_version = j.value("config_version", 2);

    // Root global (aligned to Sample PDNA_PARAMS::numSamplesPerChannel)
    s.numSamplesPerChannel = j.value("numSamplesPerChannel", 0);

    Require(!s.system_name.empty(), "system_name is required");
    Require(!s.udp_target_ip.empty(), "udp_target_ip is required");
    Require(s.udp_target_port != 0, "udp_target_port must be > 0");
    Require(s.numSamplesPerChannel > 0, "numSamplesPerChannel must be > 0");

    // UEI block (optional)
    if (j.contains("uei") && j["uei"].is_object())
    {
      const auto &ju = j["uei"];
      s.uei.iom_ip = ju.value("iom_ip", "127.0.0.1");
      s.uei.open_timeout_ms = ju.value("open_timeout_ms", 500);
      s.uei.enable_rt = ju.value("enable_rt", true);
      s.uei.rt_priority = ju.value("rt_priority", 80);
    }

    Require(j.contains("slots") && j["slots"].is_array(), "slots[] is required");

    for (const auto &js : j["slots"])
    {
      SlotConfig slot;
      slot.slot_index = js.value("slot_index", 0);
      slot.board_name = js.value("board_name", "");
      slot.active = js.value("active", false);

      slot.device_id = js.value("device_id", 0);
      slot.subsystem = js.value("subsystem", "DQ_SS0IN");
      slot.sample_rate_hz = ParseSampleRateHz(js);

      // ai_config
      if (js.contains("ai_config") && js["ai_config"].is_object())
      {
        const auto &ja = js["ai_config"];
        slot.ai_config.gain = ja.value("gain", 1);
        slot.ai_config.input_mode = ja.value("input_mode", "diff");
      }

      // channel_groups
      if (js.contains("channel_groups") && js["channel_groups"].is_array())
      {
        for (const auto &jg : js["channel_groups"])
        {
          ChannelGroupConfig g;
          g.group_name = jg.value("group_name", "");
          g.active = jg.value("active", false);
          g.target_hz = jg.value("target_hz", 0.0);

          if (jg.contains("channels") && jg["channels"].is_array())
          {
            for (const auto &ch : jg["channels"])
              g.channels.push_back(ch.get<int>());
          }

          if (jg.contains("moving_average") && jg["moving_average"].is_object())
          {
            g.moving_average.active = jg["moving_average"].value("active", false);
            g.moving_average.decimation = jg["moving_average"].value("decimation", 1);
          }

          if (jg.contains("fft") && jg["fft"].is_object())
          {
            g.fft.active = jg["fft"].value("active", false);
            g.fft.size = jg["fft"].value("size", 1024);
            g.fft.window_type = g.fft.window_type = jg["fft"].value("window_type", "hann");
            g.fft.overlap = jg["fft"].value("overlap", 0.5);
          }

          if (!g.group_name.empty())
            slot.channel_groups.push_back(g);
        }
      }

      // Validation for active slots
      if (slot.active)
      {
        Require(slot.slot_index > 0, "active slot: slot_index must be > 0");
        Require(!slot.board_name.empty(), "active slot: board_name is required");
        Require(slot.sample_rate_hz > 0.0, "active slot: sample_rate must be > 0");
        Require(!slot.ai_config.input_mode.empty(), "active slot: ai_config.input_mode is required");
        Require(!slot.channel_groups.empty(), "active slot: channel_groups[] is required");
      }

      s.slots.push_back(slot);
    }

    return s;
  }

  Settings ConfigLoader::LoadDefault()
  {
    return LoadFromFile("UEI_DAQ_Settings.json");
  }

} // namespace uei
