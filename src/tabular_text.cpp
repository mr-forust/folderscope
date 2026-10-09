#include "tabular_text.h"

QString spreadsheetSafeCell(QString value)
{
    QString safe;
    safe.reserve(value.size());
    for (const QChar ch : value) {
        const ushort code = ch.unicode();
        if (ch == u'\t') {
            safe += QStringLiteral("\\t");
        } else if (ch == u'\r') {
            safe += QStringLiteral("\\r");
        } else if (ch == u'\n') {
            safe += QStringLiteral("\\n");
        } else if (code < 0x20 || code == 0x7f) {
            safe += QStringLiteral("\\u%1").arg(int(code), 4, 16, QChar(u'0'));
        } else {
            safe += ch;
        }
    }

    qsizetype first = 0;
    while (first < safe.size()
           && (safe.at(first).isSpace() || safe.at(first).unicode() == 0xfeff))
        ++first;
    if (first < safe.size()) {
        const ushort code = safe.at(first).unicode();
        const bool formulaPrefix = code == u'=' || code == u'+' || code == u'-' || code == u'@'
            || code == 0xff1d || code == 0xff0b || code == 0xff0d || code == 0xff20;
        if (formulaPrefix)
            safe.prepend(u'\'');
    }
    return safe;
}
