#pragma once

#include <simpleble/SimpleBLE.h>

#include <string>

// Explains why Bluetooth can't be used right now, or returns "" when it's ready.
// SimpleBLE's own check reports "not enabled" for several different causes
// (on macOS: missing permission as well as Bluetooth being off).
std::string bluetoothProblem(SimpleBLE::Adapter& adapter);
