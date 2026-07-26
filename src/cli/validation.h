#pragma once

#include <QString>

namespace CliValidation
{
QString normalizeDevice(const QString &device, bool requireDevPrefix);
bool isValidDevice(const QString &device);
}
