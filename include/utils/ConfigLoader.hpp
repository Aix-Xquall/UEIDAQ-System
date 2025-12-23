//=============================================================================
// NAME:    src/utils/ConfigLoader.hpp
//=============================================================================
#pragma once

#include "utils/UeiStructs.h"
#include <string>

namespace Utils
{

    class ConfigLoader
    {
    public:
        /**
         * @brief 載入並解析 JSON 設定檔
         * * @param filePath JSON 檔案路徑 (e.g., "config/DAQ_Settings.json")
         * @return SystemConfig 解析後的系統設定結構
         * @throws std::runtime_error 若檔案不存在或格式錯誤
         */
        static SystemConfig load(const std::string &filePath);
    };

} // namespace Utils