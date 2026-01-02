#pragma once

#include <memory>
#include <vector>

#include "daq/DAQDevice.hpp"
#include "utils/UeiStructs.hpp"

namespace uei
{

    /**
     * @brief Create DAQDevice instances from Settings.
     *
     * MVP behavior:
     * - Only supports DNA-AI-217.
     * - Creates devices for groups where stream_active=true.
     * - DSP (MA/FFT) is not executed in MVP; streaming is raw.
     */
    class DAQFactory
    {
    public:
        /**
         * @brief Create devices based on settings.
         * @param settings Parsed settings.
         * @return Vector of devices (may be empty if no stream_active groups).
         * @throws std::runtime_error on unsupported config.
         */
        static std::vector<std::unique_ptr<DAQDevice>> CreateDevices(const Settings &settings);
    };

} // namespace uei
