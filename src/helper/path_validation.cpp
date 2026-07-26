#include "helper/path_validation.h"

#include <QDir>

namespace HelperPath
{
QString joinTargetPath(const QString &rootPath, const QString &path)
{
    if (rootPath.isEmpty()) {
        return path;
    }
    return QDir(rootPath).filePath(path.mid(1));
}

bool isValidRootPath(const QString &rootPath)
{
    return rootPath.isEmpty() || (QDir::isAbsolutePath(rootPath) && QDir(rootPath).exists());
}

bool isValidAbsolutePath(const QString &path)
{
    return QDir::isAbsolutePath(path) && !path.contains(QLatin1Char('\n'));
}

bool isSafeChildPath(const QString &rootPath, const QString &requestedPath)
{
    if (rootPath.isEmpty()) {
        return isValidAbsolutePath(requestedPath);
    }

    const QString canonicalRoot = QDir(rootPath).canonicalPath();
    if (canonicalRoot.isEmpty()) {
        return false;
    }

    const QString candidate = QDir::cleanPath(joinTargetPath(rootPath, requestedPath));
    return candidate == canonicalRoot || candidate.startsWith(canonicalRoot + '/');
}
}
