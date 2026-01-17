#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "daq/DAQFactory.hpp"
#include "dsp/Moving_Average.hpp"
#include "net/UdpSender.hpp"
#include "utils/ConfigLoader.hpp"
#include "utils/CsvPacketizer.hpp"
#include "utils/RingBuffer.hpp"

namespace
{

  /** @brief 簡易日誌工具。 */
  void LogInfo(const std::string &msg) { std::cout << "[INFO] " << msg << std::endl; }
  void LogWarn(const std::string &msg) { std::cerr << "[WARN] " << msg << std::endl; }
  void LogError(const std::string &msg) { std::cerr << "[ERROR] " << msg << std::endl; }

  struct GroupPlan
  {
    std::string group_name;
    std::vector<int> channels;
    std::vector<int> channel_indices;
    uei::dsp::MovingAverageConfig ma_cfg;
  };

  struct SlotPlan
  {
    int slot_index{0};
    std::vector<int> merged_channels;
    std::vector<GroupPlan> groups;
  };

  std::vector<SlotPlan> BuildSlotPlans(const uei::Settings &settings)
  {
    std::vector<SlotPlan> plans;

    for (const auto &slot : settings.slots)
    {
      if (!slot.active)
        continue;
      if (slot.board_name != "DNA-AI-217")
        continue;

      SlotPlan plan;
      plan.slot_index = slot.slot_index;

      std::unordered_set<int> seen_channels;
      bool any_group = false;

      for (const auto &g : slot.channel_groups)
      {
        if (!g.active)
          continue;
        any_group = true;

        if (g.group_name.empty())
          throw std::runtime_error("Config error: slot " + std::to_string(slot.slot_index) + " group_name empty");
        if (g.channels.empty())
          throw std::runtime_error("Config error: slot " + std::to_string(slot.slot_index) +
                                   " group '" + g.group_name + "' has empty channels");

        GroupPlan gp;
        gp.group_name = g.group_name;
        gp.channels = g.channels;
        gp.ma_cfg.active = g.moving_average.active;
        gp.ma_cfg.decimation = g.moving_average.decimation;

        for (size_t i = 0; i < g.channels.size(); ++i)
        {
          const int ch = g.channels[i];
          if (seen_channels.count(ch))
            throw std::runtime_error("Config error: slot " + std::to_string(slot.slot_index) +
                                     " channel overlap detected: ch=" + std::to_string(ch));
          seen_channels.insert(ch);
          gp.channel_indices.push_back(static_cast<int>(plan.merged_channels.size()));
          plan.merged_channels.push_back(ch);
        }

        plan.groups.push_back(gp);
      }

      if (!any_group)
        continue;

      if (plan.merged_channels.empty())
        throw std::runtime_error("Config error: slot " + std::to_string(slot.slot_index) +
                                 " merged channel list is empty");

      plans.push_back(plan);
    }

    return plans;
  }

  std::unordered_map<std::string, uei::dsp::MovingAverageConfig> BuildMovingAverageConfigsForSlot(const SlotPlan &plan)
  {
    std::unordered_map<std::string, uei::dsp::MovingAverageConfig> cfg;
    for (const auto &g : plan.groups)
      cfg[g.group_name] = g.ma_cfg;
    return cfg;
  }

  void LogStreamSummary(const uei::Settings &settings)
  {
    LogInfo("設定檔串流列表:");
    bool any = false;
    for (const auto &slot : settings.slots)
    {
      if (!slot.active)
        continue;
      for (const auto &g : slot.channel_groups)
      {
        if (!g.active)
          continue;
        const double rate_hz = (g.target_hz > 0.0) ? g.target_hz : slot.sample_rate_hz;
        std::string msg = " - Slot " + std::to_string(slot.slot_index) + ": " + slot.board_name +
                          " / " + g.group_name + " (" + std::to_string(rate_hz) + " Hz) ch=[";
        for (size_t i = 0; i < g.channels.size(); ++i)
        {
          msg += std::to_string(g.channels[i]);
          if (i + 1 < g.channels.size())
            msg += ", ";
        }
        msg += "]";
        LogInfo(msg);
        any = true;
      }
    }
    if (!any)
      LogWarn("未找到可串流的 slot/channel_groups，請確認 active 設定。");
  }

  struct Pipeline
  {
    uei::DAQDevice *dev;
    SlotPlan plan;
    uei::dsp::MovingAverageProcessor ma_processor;
    uei::UdpSender udp;
    uei::RingBuffer<uei::RawFrame> rb_raw;
    uei::RingBuffer<uei::RawFrame> rb_processed;
    std::thread th_daq;
    std::thread th_proc;
    std::thread th_udp;

    Pipeline(uei::DAQDevice *d,
             const SlotPlan &p,
             std::size_t raw_cap,
             std::size_t proc_cap)
        : dev(d), plan(p), ma_processor(BuildMovingAverageConfigsForSlot(p)), rb_raw(raw_cap), rb_processed(proc_cap)
    {
    }
  };

  /**
   * @brief 啟動 DAQ 執行緒，負責讀取 frame 並推入 raw RingBuffer。
   * @param dev 裝置。
   * @param rb_raw RawFrame 緩衝。
   * @param rb_processed 後續緩衝，用於同步停止。
   * @param stop_all 停止旗標。
   * @return 執行緒物件。
   */
  std::thread LaunchDaqThread(uei::DAQDevice &dev,
                              uei::RingBuffer<uei::RawFrame> &rb_raw,
                              uei::RingBuffer<uei::RawFrame> &rb_processed,
                              std::atomic<bool> &stop_all)
  {
    return std::thread([&]()
                       {
      while (!stop_all.load())
      {
        uei::RawFrame frame;
        if (!dev.ReadFrame(frame))
        {
          stop_all.store(true);
          rb_raw.Stop();
          rb_processed.Stop();
          break;
        }

        if (frame.samples_per_channel <= 0 || frame.num_channels <= 0 || frame.raw.empty())
          continue;

        rb_raw.Push(std::move(frame));
      } });
  }

  /**
   * @brief 啟動 Moving Average 處理執行緒。
   * @param ma_processor 移動平均處理器。
   * @param rb_raw 原始資料緩衝。
   * @param rb_processed 處理後緩衝。
   * @param stop_all 停止旗標。
   * @return 執行緒物件。
   */
  std::thread LaunchProcessingThread(uei::dsp::MovingAverageProcessor &ma_processor,
                                     const SlotPlan &plan,
                                     uei::RingBuffer<uei::RawFrame> &rb_raw,
                                     uei::RingBuffer<uei::RawFrame> &rb_processed,
                                     std::atomic<bool> &stop_all)
  {
    return std::thread([&]()
                       {
      while (!stop_all.load())
      {
        uei::RawFrame frame;
        if (!rb_raw.PopFor(frame, std::chrono::milliseconds(200)))
          continue;

        if (frame.slot_index != plan.slot_index)
        {
          LogWarn("Slot mismatch: expected slot " + std::to_string(plan.slot_index) +
                  ", got " + std::to_string(frame.slot_index));
          continue;
        }

        if (frame.num_channels != static_cast<int>(plan.merged_channels.size()))
        {
          LogWarn("Channel count mismatch: expected " + std::to_string(plan.merged_channels.size()) +
                  ", got " + std::to_string(frame.num_channels));
          continue;
        }

        for (const auto &g : plan.groups)
        {
          if (g.channel_indices.empty())
            continue;

          uei::RawFrame sub;
          sub.seq = frame.seq;
          sub.slot_index = frame.slot_index;
          sub.group_name = g.group_name;
          sub.samples_per_channel = frame.samples_per_channel;
          sub.num_channels = static_cast<int>(g.channel_indices.size());
          sub.raw.resize(static_cast<size_t>(sub.samples_per_channel * sub.num_channels));

          const int samples = sub.samples_per_channel;
          const int total_ch = frame.num_channels;
          for (int s = 0; s < samples; ++s)
          {
            const int base_in = s * total_ch;
            const int base_out = s * sub.num_channels;
            for (int i = 0; i < sub.num_channels; ++i)
            {
              const int src_idx = g.channel_indices[static_cast<size_t>(i)];
              sub.raw[static_cast<size_t>(base_out + i)] = frame.raw[static_cast<size_t>(base_in + src_idx)];
            }
          }

          try
          {
            std::vector<uei::RawFrame> outs = ma_processor.ProcessFrame(std::move(sub));
            for (auto &out : outs)
              rb_processed.Push(std::move(out));
          }
          catch (const std::exception &ex)
          {
            LogWarn(std::string("MovingAverage error: ") + ex.what());
          }
        }
      } });
  }

  /**
   * @brief 啟動 UDP 傳輸執行緒。
   * @param udp 傳輸器。
   * @param rb_processed 處理後緩衝。
   * @param stop_all 停止旗標。
   * @return 執行緒物件。
   */
  std::thread LaunchUdpThread(uei::UdpSender &udp,
                              uei::RingBuffer<uei::RawFrame> &rb_processed,
                              std::atomic<bool> &stop_all)
  {
    return std::thread([&]()
                       {
      while (!stop_all.load())
      {
        uei::RawFrame frame;
        if (!rb_processed.PopFor(frame, std::chrono::milliseconds(200)))
          continue;

        const std::string payload = uei::CsvPacketizer::Encode(frame);
        if (!udp.Send(payload.data(), payload.size()))
        {
          LogWarn("UDP send failed.");
        }
      } });
  }

} // namespace

int main()
{
  try
  {
    LogInfo("載入設定檔...");
    const uei::Settings settings = uei::ConfigLoader::LoadDefault();
    LogStreamSummary(settings);

    LogInfo("建立 DAQ 裝置...");
    const std::vector<SlotPlan> slot_plans = BuildSlotPlans(settings);
    std::vector<std::unique_ptr<uei::DAQDevice>> devices = uei::DAQFactory::CreateDevices(settings);
    if (devices.empty())
    {
      throw std::runtime_error("沒有可串流的裝置。請確認 slots[].active 與 channel_groups[].active 設定。");
    }
    if (devices.size() != slot_plans.size())
    {
      throw std::runtime_error("裝置數量與 slot 設定不一致，請確認設定與裝置建立邏輯。");
    }

    constexpr std::size_t kRawBufferFrames = 128;
    constexpr std::size_t kProcessedBufferFrames = 128;
    std::atomic<bool> stop_all{false};

    std::vector<std::unique_ptr<Pipeline>> pipelines;
    pipelines.reserve(devices.size());
    for (size_t i = 0; i < devices.size(); ++i)
    {
      pipelines.emplace_back(new Pipeline(devices[i].get(), slot_plans[i], kRawBufferFrames, kProcessedBufferFrames));
    }

    for (auto &p : pipelines)
    {
      p->udp.Open(settings.udp_target_ip, settings.udp_target_port);
      p->dev->Open();
      p->dev->Start();

      p->th_daq = LaunchDaqThread(*p->dev, p->rb_raw, p->rb_processed, stop_all);
      p->th_proc = LaunchProcessingThread(p->ma_processor, p->plan, p->rb_raw, p->rb_processed, stop_all);
      p->th_udp = LaunchUdpThread(p->udp, p->rb_processed, stop_all);
    }

    for (auto &p : pipelines)
      p->th_daq.join();

    stop_all.store(true);
    for (auto &p : pipelines)
    {
      p->rb_raw.Stop();
      p->rb_processed.Stop();
    }

    for (auto &p : pipelines)
    {
      p->th_proc.join();
      p->th_udp.join();
    }

    for (auto &p : pipelines)
    {
      p->dev->Stop();
      p->dev->Close();
      p->udp.Close();
    }

    LogInfo("程式正常結束。");
    return 0;
  }
  catch (const std::exception &e)
  {
    LogError(std::string("致命錯誤: ") + e.what());
    return 1;
  }
}
