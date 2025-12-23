//=============================================================================
// NAME:    include/daq/DaqAI217.hpp
// DESC:    AI-217 實作類別宣告
//=============================================================================
#pragma once

#include "UeiDaqDevice.hpp"

namespace Daq
{

    class DaqAI217 : public UeiDaqDevice
    {
    public:
        DaqAI217(const Utils::TaskConfig &config);
        virtual ~DaqAI217();

        // 實作介面
        bool Configure() override;

    protected:
        void DaqLoop() override;

    private:
        // 解析 Gain 設定值轉為 SDK 參數
        int GetGainCode(int gainVal);
    };

} // namespace Daq