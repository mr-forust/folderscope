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
    const QString migrationKey = QStringLiteral("migration/legacySettingsImported");
    const QString previousMigrationKey =
        QStringLiteral("migration/searchMyFilesSettingsImported");
    if (settings.value(migrationKey,
                       settings.value(previousMigrationKey, false)).toBool()) {
        settings.setValue(migrationKey, true);
        return;
    }

    // Keep the old settings namespace so existing user settings are imported.
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
