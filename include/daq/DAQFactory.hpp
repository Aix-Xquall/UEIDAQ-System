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
     * - Rule A: stream groups where slot.active && group.active.
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
