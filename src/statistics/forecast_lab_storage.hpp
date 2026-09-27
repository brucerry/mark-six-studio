#pragma once
#include "forecast_lab_ledger.hpp"

namespace marksix::statistics {
std::string encodeLabCheckpoint(const LabReplay& replay);
LabReplay decodeLabCheckpoint(std::string_view bytes);
std::string encodeLabForecast(const LabForecast& forecast);
LabForecast decodeLabForecast(std::string_view bytes);
std::string encodeLabResult(const Result& result);
Result decodeLabResult(std::string_view bytes);
std::string encodeLabControl(const LabControlReport& report);
LabControlReport decodeLabControl(std::string_view bytes);
}
