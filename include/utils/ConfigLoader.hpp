#pragma once

#include <string>
#include "utils/UeiStructs.hpp"

namespace uei
{

  /**
   * @brief JSON settings loader for UEI_DAQ_Settings.json (config_version=2).
   */
  class ConfigLoader
  {
  public:
    /**
     * @brief Load and validate settings from JSON file.
     * @param path Path to UEI_DAQ_Settings.json
     * @return Parsed Settings
     * @throws std::runtime_error on parse/validation errors
     */
    static Settings LoadFromFile(const std::string &path);

    /**
     * @brief Load settings from default path "./UEI_DAQ_Settings.json".
     * @return Parsed Settings
     * @throws std::runtime_error on errors
     */
    static Settings LoadDefault();
  };

} // namespace uei
