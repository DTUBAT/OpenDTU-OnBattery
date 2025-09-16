// SPDX-License-Identifier: GPL-2.0-or-later
#include <gridcharger/HTTP/Controller.h>
#include "PowerLimiter.h"
#include "Configuration.h"
#include "powermeter/Controller.h"
#include "battery/Controller.h"
#include <LogHelper.h>

static const char* TAG = "gridCharger";
static const char* SUBTAG = "Controller";

#include <functional>
#include <algorithm>

GridChargers::HTTP::Controller HTTPCtrl;
namespace GridChargers::HTTP {

void Controller::init(Scheduler& scheduler)
{
    ESP_LOGI(TAG, "Initializing HTTP AC charger interface...");

    scheduler.addTask(_loopTask);
    _loopTask.setCallback(std::bind(&Controller::loop, this));
    _loopTask.setIterations(TASK_FOREVER);
    _loopTask.enable();

    updateSettings();
}

void Controller::enableOutput()
{
    if (_oOutputEnabled.value_or(false)) { return; }

    _setProduction(true);
    _oOutputEnabled = true;


}

void Controller::disableOutput()
{
    if (!_oOutputEnabled.value_or(true)) { return; }

    _setProduction(false);
    _oOutputEnabled = false;


}

void Controller::updateSettings()
{
    std::lock_guard<std::mutex> lock(_mutex);



    auto const& config = Configuration.get();

    if (!config.GridCharger.Enabled || config.GridCharger.Provider != GridChargerProviderType::HTTP)
    {
        ESP_LOGI(TAG, "WEB Interface initialized error: not enabled or wrong provider");
        return;
    }


    ESP_LOGI(TAG, "WEB Interface initialized successfully");
}

void Controller::loop()
{
    std::lock_guard<std::mutex> lock(_mutex);



    auto const& config = Configuration.get();

    auto oOutputCurrent = _dataPoints.get<DataPointLabel::OutputCurrent>();
    auto oOutputVoltage = _dataPoints.get<DataPointLabel::OutputVoltage>();
    auto oOutputPower = _dataPoints.get<DataPointLabel::OutputPower>();
    auto oEfficiency = _dataPoints.get<DataPointLabel::Efficiency>();
    auto efficiency = oEfficiency ? (*oEfficiency > 50 ? *oEfficiency / 100 : 1.0) : 1.0;

    // ***********************
    // Emergency charge
    // ***********************
    auto stats = Battery.getStats();
    if (!_batteryEmergencyCharging && config.GridCharger.EmergencyChargeEnabled && stats->getImmediateChargingRequest()) {
        if (!oOutputVoltage) {
            // TODO(schlimmchen): if this situation actually occurs, this message
            // will be printed with high frequency for a prolonged time. how can
            // we deal with that?
            DTU_LOGW("Cannot perform emergency charging with unknown PSU output voltage value");
            return;
        }

        _batteryEmergencyCharging = true;

        // Set output current
        float outputCurrent = efficiency * (config.GridCharger.AutoPowerUpperPowerLimit / *oOutputVoltage);
        DTU_LOGI("Emergency Charge Output current %.02f", outputCurrent);

        return;
    }

    if (_batteryEmergencyCharging && !stats->getImmediateChargingRequest()) {
        // Battery request has changed. Set current to 0, wait for PSU to respond and then clear state
        // TODO(schlimmchen): this is repeated very often for up to (polling interval) seconds. maybe
        // trigger sending request for data immediately? otherwise implement a backoff instead.

        if (oOutputCurrent && *oOutputCurrent < 1) {
            _batteryEmergencyCharging = false;
        }
        return;
    }


}

void Controller::setFan(bool online, bool fullSpeed)
{
    std::lock_guard<std::mutex> lock(_mutex);




}

void Controller::_setProduction(bool enable)
{

}

void Controller::setProduction(bool enable)
{
    std::lock_guard<std::mutex> lock(_mutex);


    _setProduction(enable);
}





void Controller::setMode(uint8_t mode) {
    std::lock_guard<std::mutex> lock(_mutex);





}

void Controller::getJsonData(JsonVariant& root) const
{
    root["dataAge"] = millis() - _dataPoints.getLastUpdate();

    using Label = GridChargers::HTTP::DataPointLabel;

    auto oReachable = _dataPoints.get<Label::Reachable>();
    root["reachable"] = oReachable.value_or(false);

    auto oOutputPower = _dataPoints.get<Label::OutputPower>();
    auto oOutputCurrent = _dataPoints.get<Label::OutputCurrent>();
    root["producing"] = oOutputPower.value_or(0) > 10 && oOutputCurrent.value_or(0) > 0.1;



    addStringInSection<Label::BoardType>(root, "device", "boardType");
    addStringInSection<Label::Manufactured>(root, "device", "manufactured");
    addStringInSection<Label::ProductDescription>(root, "device", "productDescription");
    addStringInSection<Label::Row>(root, "device", "row");
    addStringInSection<Label::Slot>(root, "device", "slot");

    addValueInSection<Label::InputVoltage>(root, "input", "voltage");
    addValueInSection<Label::InputCurrent>(root, "input", "current");
    addValueInSection<Label::InputPower>(root, "input", "power");
    addValueInSection<Label::InputTemperature>(root, "input", "temp");
    addValueInSection<Label::InputFrequency>(root, "input", "frequency");
    addValueInSection<Label::Efficiency>(root, "input", "efficiency");

    addValueInSection<Label::OutputVoltage>(root, "output", "voltage");
    addValueInSection<Label::OutputCurrent>(root, "output", "current");
    addValueInSection<Label::OutputPower>(root, "output", "power");
    addValueInSection<Label::OutputTemperature>(root, "output", "temp");
    addValueInSection<Label::OutputCurrentMax>(root, "output", "maxCurrent");

    addValueInSection<Label::OnlineVoltage>(root, "acknowledgements", "onlineVoltage");
    addValueInSection<Label::OfflineVoltage>(root, "acknowledgements", "offlineVoltage");
    addValueInSection<Label::OnlineCurrent>(root, "acknowledgements", "onlineCurrent");
    addValueInSection<Label::OfflineCurrent>(root, "acknowledgements", "offlineCurrent");
    addValueInSection<Label::InputCurrentLimit>(root, "acknowledgements", "inputCurrentLimit");

    auto oProductionEnabled = _dataPoints.get<Label::ProductionEnabled>();
    if (oProductionEnabled) {
        addStringInSection(root, "acknowledgements", "productionEnabled", *oProductionEnabled?"yes":"no");
    }

    auto oFanOnlineFullSpeed = _dataPoints.get<Label::FanOnlineFullSpeed>();
    if (oFanOnlineFullSpeed) {
        addStringInSection(root, "acknowledgements", "fanOnlineFullSpeed", *oFanOnlineFullSpeed?"FanFullSpeed":"FanAuto");
    }

    auto oFanOfflineFullSpeed = _dataPoints.get<Label::FanOfflineFullSpeed>();
    if (oFanOfflineFullSpeed) {
        addStringInSection(root, "acknowledgements", "fanOfflineFullSpeed", *oFanOfflineFullSpeed?"FanFullSpeed":"FanAuto");
    }
}

} // namespace GridChargers::HTTPCtrl
