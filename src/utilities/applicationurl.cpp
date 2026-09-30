#include "applicationurl.h"

#include <string>
#include <QUrl>

#ifdef __EMSCRIPTEN__
#include <emscripten/val.h>
#endif

namespace Ripes {

QUrl resolveApplicationUrl(const QString& relativePath)
{
#ifdef __EMSCRIPTEN__
    const auto document = emscripten::val::global("document");
    const QUrl baseUrl(QString::fromStdString(
        document["URL"].as<std::string>()));

    return baseUrl.resolved(QUrl(relativePath));

#else

    return QUrl(relativePath);

#endif
}

}