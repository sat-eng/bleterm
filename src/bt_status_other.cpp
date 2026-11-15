#include "bt_status.h"

std::string bluetoothProblem(SimpleBLE::Adapter& adapter) {
    return adapter.bluetooth_enabled() ? "" : "Bluetooth is not enabled.";
}
