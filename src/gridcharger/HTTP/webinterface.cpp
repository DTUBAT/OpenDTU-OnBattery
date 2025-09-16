// SPDX-License-Identifier: GPL-2.0-or-later

#include <Arduino.h>
#include <Configuration.h>
#include <gridcharger/HTTP/webInterface.h>
#include <LogHelper.h>
#include <sstream>

static const char* TAG = "gridCharger";
static const char* SUBTAG = "webIfc";

namespace GridChargers::HTTP {

void webInterface::staticLoopHelper(void* context)
{
    auto pInstance = static_cast<webInterface*>(context);
    static auto constexpr resetNotificationValue = pdTRUE;
    static auto constexpr notificationTimeout = pdMS_TO_TICKS(100);

    while (true) {
        ulTaskNotifyTake(resetNotificationValue, notificationTimeout);
        {
            std::unique_lock<std::mutex> lock(pInstance->_mutex);
            if (pInstance->_stopLoop) { break; }
            pInstance->loop();
        }
    }

    pInstance->_taskDone = true;

    vTaskDelete(nullptr);
}

bool webInterface::startLoop()
{
    uint32_t constexpr stackSize = 4096;
    return pdPASS == xTaskCreate(webInterface::staticLoopHelper,
            "HTTPwebIfc", stackSize, this, 16/*prio*/, &_taskHandle);
}

void webInterface::stopLoop()
{
    if (_taskHandle == nullptr) { return; }

    _taskDone = false;

    {
        std::unique_lock<std::mutex> lock(_mutex);
        _stopLoop = true;
    }

    xTaskNotifyGive(_taskHandle);

    while (!_taskDone) { delay(10); }
    _taskHandle = nullptr;
}





bool webInterface::readRectifierState()
{

            _upData->add<DataPointLabel::InputPower>(155.5f);


    return true;
}

bool webInterface::readAcks(can_message_t const& msg)
{
    if (msg.canId != 0x1081807e) { return false; }

    uint32_t valueId = msg.valueId;

    auto setting = static_cast<Setting>(valueId >> 16);
    auto flags = valueId & 0x0000FFFF;

    float divisor = 1024;

    if (setting == Setting::OnlineCurrent || setting == Setting::OfflineCurrent) {
        if (_maxCurrentMultiplier == 0) {
            DTU_LOGW("cannot process %s current setting while respective multiplier unknown",
                    setting == Setting::OnlineCurrent ? "online" : "offline");
            return false;
        }
        divisor = _maxCurrentMultiplier;
    }

    float value = static_cast<float>(msg.value)/divisor;

    switch (setting) {
        case Setting::OnlineVoltage:
            _upData->add<DataPointLabel::OnlineVoltage>(value);
            break;
        case Setting::OfflineVoltage:
            _upData->add<DataPointLabel::OfflineVoltage>(value);
            break;
        case Setting::OnlineCurrent:
            _upData->add<DataPointLabel::OnlineCurrent>(value);
            break;
        case Setting::OfflineCurrent:
            _upData->add<DataPointLabel::OfflineCurrent>(value);
            break;
        case Setting::InputCurrentLimit:
            _upData->add<DataPointLabel::InputCurrentLimit>(value);
            break;
        case Setting::ProductionDisable:
            _upData->add<DataPointLabel::ProductionEnabled>((flags & 0x0001) == 0);
            break;
        case Setting::FanOnlineFullSpeed:
            _upData->add<DataPointLabel::FanOnlineFullSpeed>((flags & 0x0001) > 0);
            break;
        case Setting::FanOfflineFullSpeed:
            _upData->add<DataPointLabel::FanOfflineFullSpeed>((flags & 0x0001) > 0);
            break;
    }

    return true;
}

void webInterface::sendSettings()
{
    auto const& config = Configuration.get().GridCharger;

    using Setting = webInterface::Setting;
    enqueueParameter(Setting::OfflineVoltage, config.Huawei.OfflineVoltage);
    enqueueParameter(Setting::OfflineCurrent, config.Huawei.OfflineCurrent);
    enqueueParameter(Setting::InputCurrentLimit, config.Huawei.InputCurrentLimit);
    enqueueParameter(Setting::FanOnlineFullSpeed, config.Huawei.FanOnlineFullSpeed ? 1 : 0);
    enqueueParameter(Setting::FanOfflineFullSpeed, config.Huawei.FanOfflineFullSpeed ? 1 : 0);

    _lastSettingsUpdateMillis = millis();
}

void webInterface::logMessage(char const* msg, uint32_t canId, uint32_t valueId, uint32_t value)
{
    if (!DTU_LOG_IS_VERBOSE) { return; }

    char buffer[70] = {0};
    int offset = 0;
    auto save_snprintf = [&offset, &buffer](auto&&... args) -> bool {
        int written = snprintf(buffer + offset, sizeof(buffer) - offset, std::forward<decltype(args)>(args)...);
        if (written < 0 || written >= static_cast<int>(sizeof(buffer)) - offset) {
            DTU_LOGE("snprintf issue: wrote %d bytes, offset is %d, buffer size is %d, buffered '%s'",
                    written, offset, sizeof(buffer), buffer);
            return false;
        }
        offset += written;
        return true;
    };
    if (!save_snprintf("%9s: address %08x ID %08x value %08x | ",
            msg, canId, valueId, value)) { return; }

    auto printAscii = [&save_snprintf](uint32_t value) -> bool{
        for (int i = 24; i >= 0; i -= 8) {
            uint8_t byte = (value >> i) & 0xFF;
            if (byte >= 0x20 && byte <= 0x7E) {
                if (!save_snprintf("%c", byte)) { return false; }
            } else {
                if (!save_snprintf(".")) { return false; }
            }
        }
        return true;
    };

    if (!printAscii(valueId)) { return; }
    if (!save_snprintf(" ")) { return; }
    if (!printAscii(value)) { return; }

    DTU_LOGV("%s", buffer);
}

void webInterface::loop()
{
    if (!_upData) { _upData = std::make_unique<DataPointContainerHTTP>(); }


        readRectifierState();

    // the first thing we need to do is to request the device config so we know
    // the max current multiplier. that is required to process the ACK for
    // the offline current setting, or even send that setting in particular.
    if (!_lastDeviceConfigMillis) {
        // stand by while processing the device config request and not timed out
        if ((millis() - _lastRequestMillis) < 5000) { return; }


        _lastRequestMillis = millis();

        return ;
    }

    if ((millis() - *_lastDeviceConfigMillis) > DeviceConfigTimeoutMillis) {
        DTU_LOGW("PSU is unreachable (no CAN communication)");
        _maxCurrentMultiplier = 0;
        _lastDeviceConfigMillis = std::nullopt;
        _lastSettingsUpdateMillis = std::nullopt;
        _boardPropertiesState = StringState::Unknown;
        _upData->add<DataPointLabel::Reachable>(false, true/*ignore age*/);
        return; // restart by re-requesting device config in next iteration
    }

    if (!_lastSettingsUpdateMillis) {
        sendSettings();
        return processQueue();
    }

    if (StringState::Complete != _boardPropertiesState) {
        // stand by while processing the board properties replies and not timed out
        if (StringState::Reading == _boardPropertiesState &&
                (millis() - _lastRequestMillis) < 5000) { return; }

        _lastRequestMillis = millis();

        _sendQueue.push(command_t {
            .tries = 1,
            .deviceAddress = 1,
            .registerAddress = 0xD2FE,
            .command = 0,
            .flags = 0,
            .value = 0
        });

        return processQueue(); // not sending timed requests until we know the board properties
    }

    if ((millis() - _lastRequestMillis) >= DataRequestIntervalMillis) {
        // we request the device config regularly as the row and index (slot detection)
        // might change by use of the "power" pin, and we use the device config to
        // determine whether the PSU is reachable.
        requestDeviceConfig();

        // request rectifier state
        _sendQueue.push(command_t {
            .tries = 1,
            .deviceAddress = 1,
            .registerAddress = 0x40FE,
            .command = 0,
            .flags = 0,
            .value = 0
        });

        _lastRequestMillis = millis();

        return processQueue();
    }
}

void webInterface::processQueue()
{
    size_t queueSize = _sendQueue.size();
    for (size_t i = 0; i < queueSize; ++i) {
        auto& cmd = _sendQueue.front();

        std::array<uint8_t, 8> data = {
            static_cast<uint8_t>((cmd.command >> 8) & 0xFF),
            static_cast<uint8_t>((cmd.command >> 0) & 0xFF),
            static_cast<uint8_t>((cmd.flags >>  8) & 0xFF),
            static_cast<uint8_t>((cmd.flags >>  0) & 0xFF),
            static_cast<uint8_t>((cmd.value >> 24) & 0xFF),
            static_cast<uint8_t>((cmd.value >> 16) & 0xFF),
            static_cast<uint8_t>((cmd.value >>  8) & 0xFF),
            static_cast<uint8_t>((cmd.value >>  0) & 0xFF)
        };

        uint32_t addr = 0x10800000 | (cmd.deviceAddress << 16) | cmd.registerAddress;

        uint32_t valueId = (static_cast<uint32_t>(cmd.command) << 16) | cmd.flags;
        logMessage("sending", addr, valueId, cmd.value);

        if (sendMessage(addr, data)) {
            _sendQueue.pop();
            continue;
        }

        if (cmd.tries > 0) { --cmd.tries; }

        DTU_LOGE("Sending to 0x%08x failed (no CAN ACK), command 0x%04x, "
                "flags 0x%04x, value 0x%08x, %d tries remaining",
                addr, cmd.command, cmd.flags, cmd.value, cmd.tries);

        if (cmd.tries == 0) { _sendQueue.pop(); }
    }
}

void webInterface::enqueueParameter(webInterface::Setting setting, float val)
{
    uint16_t flags = 0;

    switch (setting) {
        case Setting::OfflineVoltage:
        case Setting::OnlineVoltage:
            val *= 1024;
            break;
        case Setting::OfflineCurrent:
        case Setting::OnlineCurrent:
            if (_maxCurrentMultiplier == 0) {
                DTU_LOGW("max current multiplier unknown, cannot send current setting");
                return;
            }
            val *= _maxCurrentMultiplier;
            break;
        case Setting::InputCurrentLimit:
            val *= 1024;
            if (val > 0) {
                flags = 0x0001;
            }
            break;
        case Setting::FanOnlineFullSpeed:
        case Setting::FanOfflineFullSpeed:
        case Setting::ProductionDisable:
            if (val > 0) {
                flags = 0x0001;
            }
            val = 0;
            break;
    }

    _sendQueue.push(command_t {
        .tries = 3,
        .deviceAddress = 1,
        .registerAddress = 0x80FE,
        .command = static_cast<uint16_t>(setting),
        .flags = flags,
        .value = static_cast<uint32_t>(val)
    });
}

void webInterface::setParameter(webInterface::Setting setting, float val, bool pollFeedback)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_taskHandle == nullptr) { return; }

    enqueueParameter(setting, val);

    if (pollFeedback) { // request early param feedback
        _lastRequestMillis = millis() - DataRequestIntervalMillis;
    }

    xTaskNotifyGive(_taskHandle);
}

std::unique_ptr<DataPointContainerHTTP> webInterface::getCurrentData()
{
    std::unique_ptr<DataPointContainerHTTP> upData = nullptr;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        upData = std::move(_upData);
    }

    if (upData && DTU_LOG_IS_DEBUG) {
        auto iter = upData->cbegin();
        while (iter != upData->cend()) {
            DTU_LOGD("[%.3f] %s: %s%s",
                static_cast<float>(iter->second.getTimestamp())/1000,
                iter->second.getLabelText().c_str(),
                iter->second.getValueText().c_str(),
                iter->second.getUnitText().c_str());
            ++iter;
        }
    }

    return std::move(upData);
}

} // namespace GridChargers::HTTP
