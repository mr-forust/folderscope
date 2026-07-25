#include <QApplication>
#include <QMetaType>
#include <QTimer>

#include "main_window.h"
#include "search_core.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SearchMyFilesLinux"));
    QCoreApplication::setApplicationName(QStringLiteral("SearchMyFiles"));
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
