#include "cli/validation.h"

namespace CliValidation
{
QString normalizeDevice(const QString &device, bool requireDevPrefix)
{
    if (device.isEmpty()) {
        return device;
    }
    if (requireDevPrefix) {
        return device.startsWith("/dev/") ? device : "/dev/" + device;
    }
    return device.startsWith("/dev/") ? device.mid(5) : device;
}

bool isValidDevice(const QString &device)
{
    if (device.isEmpty()) {
        return true;
    }
    if (device.startsWith("/dev/")) {
        const QString deviceName = device.mid(5);
        return !deviceName.isEmpty() && !deviceName.contains('/');
    }
    return !device.contains('/');
}
}
