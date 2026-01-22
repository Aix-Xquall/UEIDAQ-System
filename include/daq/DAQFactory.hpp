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
     * Supported behavior:
     * - Supports DNA-AI-217 and DNA-AI-211.
     * - One device per slot; channels merged from active groups.
     */
    class DAQFactory
    {
    public:
        /**
         * @brief Create devices based on settings.
         * @param settings Parsed settings.
         * @return Vector of devices.
         */
        static std::vector<std::unique_ptr<DAQDevice>> CreateDevices(const Settings &settings);
    };

} // namespace uei
