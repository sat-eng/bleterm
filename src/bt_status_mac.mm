#include "bt_status.h"

#import <CoreBluetooth/CoreBluetooth.h>

// macOS records the permission for the app that launched bleterm (iTerm2, CLion...), unless
// bleterm was built with BLETERM_OWN_BLUETOOTH_IDENTITY, which gives it its own (local.bleterm).
static const char* PERMISSION_HELP =
    " Turn on the app you run bleterm from (iTerm2, CLion...) in System Settings > Privacy &"
    " Security > Bluetooth, then restart that app. If it isn't listed, run"
    " 'tccutil reset BluetoothAlways <its bundle id, e.g. com.googlecode.iterm2>' and start"
    " bleterm again to get the prompt.";

std::string bluetoothProblem(SimpleBLE::Adapter& adapter) {
    switch (CBCentralManager.authorization) {
        case CBManagerAuthorizationNotDetermined:
            return std::string("Bluetooth permission hasn't been granted yet.") + PERMISSION_HELP;
        case CBManagerAuthorizationDenied:
            return std::string("Bluetooth permission is denied.") + PERMISSION_HELP;
        case CBManagerAuthorizationRestricted:
            return "Bluetooth use is restricted on this Mac (for example by a management profile).";
        default:
            break;
    }

    // On macOS SimpleBLE's underlying adapter object is its CBCentralManager.
    CBCentralManager* manager = (__bridge CBCentralManager*)adapter.underlying();
    switch (manager.state) {
        case CBManagerStatePoweredOn: return "";
        case CBManagerStatePoweredOff: return "Bluetooth is switched off.";
        case CBManagerStateUnauthorized: return std::string("Bluetooth permission is denied.") + PERMISSION_HELP;
        case CBManagerStateUnsupported: return "This Mac doesn't support Bluetooth LE.";
        case CBManagerStateResetting: return "Bluetooth is resetting; try again in a moment.";
        default: return "Bluetooth isn't ready yet; try again in a moment.";
    }
}
