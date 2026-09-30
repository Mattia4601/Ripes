#include "applicationurl.h"

#include <QUrl>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
// funzione in javascript che recupera l'url della pagina web in cui è eseguito Ripes
// gli argomenti di EM_JS sono: il tipo di ritorno, il nome della funzione, la lista degli argomenti, il corpo della funzione
EM_JS(char*, getPageUrl, (), {
    const url = window.location.href;
    const length = lengthBytesUTF8(url) + 1;
    const buffer = _malloc(length);
    stringToUTF8(url, buffer, length);
    return buffer;
});
#endif

namespace Ripes {

QUrl resolveApplicationUrl(const QString& relativePath)
{
#ifdef __EMSCRIPTEN__

    char* pageUrl = getPageUrl();

    QString currentUrl = QString::fromUtf8(pageUrl);

    free(pageUrl);

    QUrl baseUrl(currentUrl);
    // prende un url relativo e lo risolve rispetto all'url della pagina web in cui è eseguito Ripes
    return baseUrl.resolved(QUrl(relativePath));

#else

    return QUrl(relativePath);

#endif
}

}