#include <QApplication>
#include <QDir>
#include <QMetaType>
#include <QLocale>
#include <QSettings>
#include <QTimer>
#include <QTranslator>

#include "main_window.h"
#include "search_core.h"

namespace {

void migrateLegacySettings()
{
    QSettings settings;
    const QString migrationKey =
        QStringLiteral("migration/searchMyFilesSettingsImported");
    if (settings.value(migrationKey, false).toBool())
        return;

    QSettings legacy(QStringLiteral("SearchMyFilesLinux"),
                     QStringLiteral("SearchMyFiles"));
    for (const QString &key : legacy.allKeys()) {
        if (!settings.contains(key))
            settings.setValue(key, legacy.value(key));
    }
    settings.setValue(migrationKey, true);
}

}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("FolderScope"));
    QCoreApplication::setApplicationName(QStringLiteral("FolderScope"));
    QTranslator translator;
    const QString translationsPath = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("../share/folderscope/translations"));
    if ((translator.load(QLocale(), QStringLiteral("folderscope"),
                         QStringLiteral("_"), translationsPath)
         || translator.load(QLocale(), QStringLiteral("folderscope"),
                            QStringLiteral("_"), QStringLiteral(":/i18n"))))
        app.installTranslator(&translator);
    migrateLegacySettings();
    QApplication::setStyle(QStringLiteral("Fusion"));
    qRegisterMetaType<SearchOptions>();
    qRegisterMetaType<QVector<FileRecord>>();
    MainWindow window;
    window.show();
    QTimer::singleShot(0, &window, [&window] {
        QMetaObject::invokeMethod(&window, "showOptions", Qt::QueuedConnection);
    });
    return app.exec();
}
