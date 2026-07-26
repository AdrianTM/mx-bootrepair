#pragma once

#include <QString>

namespace HelperPath
{
QString joinTargetPath(const QString &rootPath, const QString &path);
bool isValidRootPath(const QString &rootPath);
bool isValidAbsolutePath(const QString &path);
bool isSafeChildPath(const QString &rootPath, const QString &requestedPath);
}
