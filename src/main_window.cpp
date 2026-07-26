#include "main_window.h"

#include <QActionGroup>
#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

#include "options_dialog.h"
#include "search_engine.h"

MainWindow::MainWindow()
{
    loadOptions();
    setWindowTitle(tr("SearchMyFiles for Linux"));
    resize(1280, 760);
    model_ = new ResultsModel(this);
    model_->setDisplayOptions(sizeUnit_, showGmt_, markDuplicates_, duplicateColorSet_);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(Qt::EditRole);
    table_ = new QTableView;
    table_->setModel(proxy_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setSortingEnabled(true);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->horizontalHeader()->setSectionResizeMode(ResultsModel::Name, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(ResultsModel::Folder, QHeaderView::Stretch);
    setCentralWidget(table_);
    {
        QSettings settings;
        restoreGeometry(settings.value(QStringLiteral("ui/windowGeometry")).toByteArray());
        table_->horizontalHeader()->restoreState(
            settings.value(QStringLiteral("ui/headerState")).toByteArray());
        table_->setShowGrid(showGrid_);
        table_->viewport()->setAttribute(Qt::WA_AlwaysShowToolTips, showTooltips_);
    }

    auto *toolbar = addToolBar(tr("Main"));
    searchAction_ = toolbar->addAction(QIcon::fromTheme(QStringLiteral("system-search")), tr("Search options…"));
    searchAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F9));
    stopAction_ = toolbar->addAction(QIcon::fromTheme(QStringLiteral("process-stop")), tr("Stop"));
    stopAction_->setShortcut(Qt::Key_Escape);
    stopAction_->setEnabled(false);
    refreshAction_ = toolbar->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), tr("Refresh"));
    refreshAction_->setShortcut(Qt::Key_F5);
    toolbar->addSeparator();
    auto *exportAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("document-save")), tr("Export…"));
    exportAction->setShortcut(QKeySequence::Save);
    auto *openAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("document-open")), tr("Open"));
    openAction->setShortcut(Qt::Key_F9);
    auto *folderAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Open folder"));
    folderAction->setShortcut(Qt::Key_F8);
    auto *openWithAction = new QAction(tr("Open With…"), this);
    openWithAction->setShortcut(Qt::Key_F7);
    auto *selectInFolderAction = new QAction(tr("Select File In File Manager"), this);
    auto *trashAction = new QAction(tr("Move To Trash"), this);
    trashAction->setShortcut(Qt::Key_Delete);
    auto *deleteAction = new QAction(tr("Delete Selected Files"), this);
    deleteAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    auto *renameAction = new QAction(tr("Rename"), this);
    renameAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    auto *propertiesAction = new QAction(tr("Properties"), this);
    propertiesAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
    for (QAction *action : {openWithAction, selectInFolderAction, trashAction,
                            deleteAction, renameAction, propertiesAction})
        addAction(action);

    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(searchAction_);
    fileMenu->addAction(stopAction_);
    fileMenu->addAction(refreshAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(openAction);
    fileMenu->addAction(folderAction);
    fileMenu->addAction(openWithAction);
    fileMenu->addAction(selectInFolderAction);
    fileMenu->addSeparator();
    fileMenu->addAction(trashAction);
    fileMenu->addAction(deleteAction);
    fileMenu->addAction(renameAction);
    fileMenu->addSeparator();
    fileMenu->addAction(exportAction);
    fileMenu->addAction(propertiesAction);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Exit"), QKeySequence::Quit, this, &QWidget::close);

    auto *editMenu = menuBar()->addMenu(tr("&Edit"));
    auto *findAction = editMenu->addAction(tr("Find…"));
    findAction->setShortcut(QKeySequence::Find);
    auto *findNextAction = editMenu->addAction(tr("Find next"));
    findNextAction->setShortcut(Qt::Key_F3);
    editMenu->addSeparator();
    auto *copyInfoAction = editMenu->addAction(tr("Copy files information"));
    copyInfoAction->setShortcut(QKeySequence::Copy);
    auto *copyPathsAction = editMenu->addAction(tr("Copy full filenames path"));
    copyPathsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    auto *explorerCopyAction = editMenu->addAction(tr("Explorer copy"));
    explorerCopyAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    auto *explorerCutAction = editMenu->addAction(tr("Explorer cut"));
    explorerCutAction->setShortcut(QKeySequence::Cut);
    editMenu->addSeparator();
    editMenu->addAction(tr("Select all"), QKeySequence::SelectAll, table_, &QTableView::selectAll);
    auto *deselectAction = editMenu->addAction(tr("Deselect all"));
    deselectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));

    auto *viewMenu = menuBar()->addMenu(tr("&View"));
    auto *gridAction = viewMenu->addAction(tr("Show grid lines"));
    gridAction->setCheckable(true);
    gridAction->setChecked(showGrid_);
    auto *tooltipsAction = viewMenu->addAction(tr("Show tooltips"));
    tooltipsAction->setCheckable(true);
    tooltipsAction->setChecked(showTooltips_);
    viewMenu->addSeparator();
    auto *autoSizeAction = viewMenu->addAction(tr("Auto size columns"), this, [this] {
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table_->resizeColumnsToContents();
        saveUiSettings();
    });
    autoSizeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Plus));
    auto *columnsMenu = viewMenu->addMenu(tr("Choose columns"));
    for (int column = 0; column < ResultsModel::ColumnCount; ++column) {
        auto *action = columnsMenu->addAction(
            model_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
        action->setCheckable(true);
        action->setChecked(!table_->isColumnHidden(column));
        connect(action, &QAction::toggled, this, [this, column](bool visible) {
            table_->setColumnHidden(column, !visible);
            saveUiSettings();
        });
    }

    auto *optionsMenu = menuBar()->addMenu(tr("&Options"));
    const auto addChoiceMenu = [this, optionsMenu](
                                   const QString &title,
                                   const QStringList &choices,
                                   const QString &selected,
                                   const std::function<void(const QString &)> &changed) {
        auto *menu = optionsMenu->addMenu(title);
        auto *group = new QActionGroup(menu);
        group->setExclusive(true);
        for (const QString &choice : choices) {
            auto *action = menu->addAction(choice);
            action->setCheckable(true);
            action->setChecked(choice == selected);
            group->addAction(action);
            connect(action, &QAction::triggered, this, [this, choice, changed] {
                changed(choice);
                saveUiSettings();
            });
        }
        return menu;
    };

    addChoiceMenu(tr("File Size Unit"),
                  {tr("Automatic"), tr("Bytes"), tr("KB"), tr("MB"), tr("GB")},
                  sizeUnitName(), [this](const QString &choice) {
        sizeUnit_ = sizeUnitFromName(choice);
        applyDisplayOptions();
    });
    addChoiceMenu(tr("Summary File Size Unit"),
                  {tr("Automatic"), tr("Bytes"), tr("KB"), tr("MB"), tr("GB")},
                  summarySizeUnitName_, [this](const QString &choice) {
        summarySizeUnitName_ = choice;
        if (options_.mode == QStringLiteral("Summary"))
            applyDisplayOptions();
    });

    auto *duplicateOptions = optionsMenu->addMenu(tr("Duplicate Search Options"));
    auto *duplicateOptionsGroup = new QActionGroup(duplicateOptions);
    auto *showAllDuplicates = duplicateOptions->addAction(tr("Show All Files"));
    auto *showOnlyDuplicates = duplicateOptions->addAction(tr("Show Only Duplicate Files"));
    for (QAction *action : {showAllDuplicates, showOnlyDuplicates}) {
        action->setCheckable(true);
        duplicateOptionsGroup->addAction(action);
    }
    showOnlyDuplicates->setChecked(options_.showDuplicateCopiesOnly);
    showAllDuplicates->setChecked(!options_.showDuplicateCopiesOnly);
    connect(showOnlyDuplicates, &QAction::triggered, this, [this] {
        options_.showDuplicateCopiesOnly = true;
        saveOptions();
    });
    connect(showAllDuplicates, &QAction::triggered, this, [this] {
        options_.showDuplicateCopiesOnly = false;
        saveOptions();
    });

    addChoiceMenu(tr("Duplicate Mark Color Set"),
                  {tr("Color Set 1"), tr("Color Set 2")},
                  duplicateColorSet_ == 1 ? tr("Color Set 1") : tr("Color Set 2"),
                  [this](const QString &choice) {
        duplicateColorSet_ = choice.endsWith(u'1') ? 1 : 2;
        applyDisplayOptions();
    });
    addChoiceMenu(tr("Double-Click Action"), actionChoices(), doubleClickAction_,
                  [this](const QString &choice) { doubleClickAction_ = choice; });
    addChoiceMenu(tr("Enter Key Action"), actionChoices(), enterKeyAction_,
                  [this](const QString &choice) { enterKeyAction_ = choice; });

    auto addToggle = [this, optionsMenu](const QString &label, bool checked,
                                         const std::function<void(bool)> &changed) {
        auto *action = optionsMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(checked);
        connect(action, &QAction::toggled, this, [this, changed](bool value) {
            changed(value);
            saveUiSettings();
        });
        return action;
    };
    addToggle(tr("Include Subfolders in Summary Totals"), options_.includeSubfoldersInSummary,
              [this](bool value) { options_.includeSubfoldersInSummary = value; saveOptions(); });
    addToggle(tr("Show Time In GMT"), showGmt_, [this](bool value) {
        showGmt_ = value; applyDisplayOptions();
    });
    optionsMenu->addSeparator();
    addToggle(tr("Mark Duplicate Files"), markDuplicates_, [this](bool value) {
        markDuplicates_ = value; applyDisplayOptions();
    });
    addToggle(tr("Add Header Line To CSV/Tab-Delimited File"), addExportHeader_,
              [this](bool value) { addExportHeader_ = value; });
    addToggle(tr("Retrieve File Owner"), options_.retrieveOwner,
              [this](bool value) { options_.retrieveOwner = value; saveOptions(); });
    addToggle(tr("Accurate Progress (Count Files First)"), options_.accurateProgress,
              [this](bool value) { options_.accurateProgress = value; saveOptions(); });
    addToggle(tr("Use Persistent Search Cache"), options_.useCache,
              [this](bool value) { options_.useCache = value; saveOptions(); });
    optionsMenu->addAction(tr("Clear Search Cache"), this, [this] {
        emit clearCacheRequested();
    });
    addToggle(tr("Set Focus On Search Start"), focusOnSearchStart_,
              [this](bool value) { focusOnSearchStart_ = value; });
    addToggle(tr("Set Focus On Search End"), focusOnSearchEnd_,
              [this](bool value) { focusOnSearchEnd_ = value; });
    addToggle(tr("Auto Size Columns On Search End"), autoSizeOnSearchEnd_,
              [this](bool value) { autoSizeOnSearchEnd_ = value; });

    menuBar()->addMenu(tr("&Help"))->addAction(tr("About"), this, [this] {
        QMessageBox::about(this, tr("About"),
            tr("FolderScope %1\n\nFast unindexed file search.\nC++20 + Qt 6.")
                .arg(QStringLiteral(FOLDERSCOPE_VERSION)));
    });

    staleSearchWarning_ = new QLabel(
        tr("⚠ Search settings changed — press F5 to rescan"));
    staleSearchWarning_->setStyleSheet(
        QStringLiteral("QLabel { color: #f0b429; font-weight: 600; padding: 0 6px; }"));
    staleSearchWarning_->setToolTip(
        tr("Displayed results were produced with different search settings."));
    staleSearchWarning_->hide();
    statusBar()->addPermanentWidget(staleSearchWarning_);

    progress_ = new QProgressBar;
    progress_->setRange(0, 0);
    progress_->setMinimumWidth(280);
    progress_->setMaximumWidth(420);
    progress_->setTextVisible(true);
    progress_->hide();
    statusBar()->addPermanentWidget(progress_);
    statusBar()->showMessage(tr("Press Ctrl+F9 to configure and start a search"));

    engine_ = new SearchEngine;
    engine_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, engine_, &QObject::deleteLater);
    connect(this, &MainWindow::startRequested, engine_, &SearchEngine::search);
    connect(this, &MainWindow::stopRequested, engine_, &SearchEngine::stop, Qt::DirectConnection);
    connect(this, &MainWindow::clearCacheRequested, engine_, &SearchEngine::clearCache);
    connect(engine_, &SearchEngine::cacheCleared, this, [this] {
        statusBar()->showMessage(tr("Search cache cleared"), 5000);
    });
    connect(engine_, &SearchEngine::progress, this, [this](const QString &path, quint64 count) {
        if (isStopping_)
            return;
        progress_->setRange(0, 0);
        progress_->setFormat(tr("Scanning… %1 entries").arg(count));
        statusBar()->showMessage(tr("Scanning %1 (%2 entries)").arg(path).arg(count));
    });
    connect(engine_, &SearchEngine::phaseProgress, this,
            [this](const QString &phase, quint64 completed, quint64 total) {
        if (isStopping_)
            return;
        if (total == 0) {
            progress_->setRange(0, 0);
            progress_->setFormat(completed
                ? tr("%1… %2").arg(phase).arg(completed)
                : phase);
            statusBar()->showMessage(completed
                ? tr("%1: %2").arg(phase).arg(completed)
                : phase);
            return;
        }
        const int percent = int(std::min<quint64>(100, completed * 100 / total));
        progress_->setRange(0, 100);
        progress_->setValue(percent);
        progress_->setFormat(tr("%1% — %2").arg(percent).arg(phase));
        statusBar()->showMessage(
            tr("%1: %2 / %3").arg(phase).arg(completed).arg(total));
    });
    connect(engine_, &SearchEngine::finished, this, &MainWindow::searchFinished);
    workerThread_.start();

    connect(searchAction_, &QAction::triggered, this, &MainWindow::showOptions);
    connect(stopAction_, &QAction::triggered, this, [this] {
        isStopping_ = true;
        stopAction_->setEnabled(false);
        progress_->setRange(0, 0);
        progress_->setFormat(tr("Stopping…"));
        statusBar()->showMessage(tr("Stopping search…"));
        emit stopRequested();
    });
    connect(refreshAction_, &QAction::triggered, this, &MainWindow::refreshSearch);
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportResults);
    connect(openAction, &QAction::triggered, this, &MainWindow::openSelected);
    connect(folderAction, &QAction::triggered, this, &MainWindow::openFolder);
    connect(openWithAction, &QAction::triggered, this, &MainWindow::openWith);
    connect(selectInFolderAction, &QAction::triggered, this, &MainWindow::selectInFileManager);
    connect(trashAction, &QAction::triggered, this, &MainWindow::moveToTrash);
    connect(deleteAction, &QAction::triggered, this, &MainWindow::deleteSelected);
    connect(renameAction, &QAction::triggered, this, &MainWindow::renameSelected);
    connect(propertiesAction, &QAction::triggered, this, &MainWindow::showProperties);
    connect(findAction, &QAction::triggered, this, &MainWindow::findText);
    connect(findNextAction, &QAction::triggered, this, &MainWindow::findNext);
    connect(copyInfoAction, &QAction::triggered, this, &MainWindow::copyInformation);
    connect(copyPathsAction, &QAction::triggered, this, &MainWindow::copyPaths);
    connect(explorerCopyAction, &QAction::triggered, this, &MainWindow::explorerCopy);
    connect(explorerCutAction, &QAction::triggered, this, &MainWindow::explorerCut);
    connect(deselectAction, &QAction::triggered, table_, &QTableView::clearSelection);
    connect(gridAction, &QAction::toggled, this, [this](bool enabled) {
        showGrid_ = enabled;
        table_->setShowGrid(enabled);
        saveUiSettings();
    });
    connect(tooltipsAction, &QAction::toggled, this, [this](bool enabled) {
        showTooltips_ = enabled;
        table_->viewport()->setAttribute(Qt::WA_AlwaysShowToolTips, enabled);
        saveUiSettings();
    });
    connect(table_, &QTableView::doubleClicked, this, [this] {
        executeConfiguredAction(doubleClickAction_);
    });
    connect(table_, &QWidget::customContextMenuRequested, this, &MainWindow::contextMenu);
    connect(table_->horizontalHeader(), &QHeaderView::customContextMenuRequested,
            this, &MainWindow::headerContextMenu);
}

MainWindow::~MainWindow()
{
    emit stopRequested();
    workerThread_.quit();
    workerThread_.wait();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveUiSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && event->modifiers() == Qt::NoModifier) {
        executeConfiguredAction(enterKeyAction_);
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::showOptions()
{
    OptionsDialog dialog(options_, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    options_ = dialog.options();
    saveOptions();
    startCurrentSearch();
}

void MainWindow::refreshSearch()
{
    if (!isSearching_)
        startCurrentSearch();
}

void MainWindow::findText()
{
    bool accepted = false;
    const QString query = QInputDialog::getText(
        this, tr("Find"), tr("Find text:"), QLineEdit::Normal, findQuery_, &accepted);
    if (!accepted || query.isEmpty())
        return;
    findQuery_ = query;
    findNext();
}

void MainWindow::findNext()
{
    if (findQuery_.isEmpty() || proxy_->rowCount() == 0)
        return;
    int start = 0;
    const QModelIndex current = table_->currentIndex();
    if (current.isValid())
        start = (current.row() + 1) % proxy_->rowCount();
    for (int offset = 0; offset < proxy_->rowCount(); ++offset) {
        const int row = (start + offset) % proxy_->rowCount();
        QStringList values;
        for (int column = 0; column < proxy_->columnCount(); ++column)
            values.push_back(proxy_->index(row, column).data(Qt::DisplayRole).toString());
        if (values.join(u'\t').contains(findQuery_, Qt::CaseInsensitive)) {
            const QModelIndex match = proxy_->index(row, 0);
            table_->selectionModel()->select(
                match, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            table_->setCurrentIndex(match);
            table_->scrollTo(match, QAbstractItemView::PositionAtCenter);
            return;
        }
    }
    statusBar()->showMessage(tr("Text not found: %1").arg(findQuery_), 4000);
}

void MainWindow::copyInformation()
{
    QModelIndexList selected = table_->selectionModel()->selectedRows();
    std::sort(selected.begin(), selected.end(),
              [](const QModelIndex &a, const QModelIndex &b) { return a.row() < b.row(); });
    if (selected.isEmpty())
        return;
    QStringList lines;
    QStringList headers;
    for (int column = 0; column < proxy_->columnCount(); ++column) {
        if (!table_->isColumnHidden(column))
            headers.push_back(proxy_->headerData(column, Qt::Horizontal).toString());
    }
    lines.push_back(headers.join(u'\t'));
    for (const QModelIndex &rowIndex : selected) {
        QStringList values;
        for (int column = 0; column < proxy_->columnCount(); ++column) {
            if (!table_->isColumnHidden(column))
                values.push_back(proxy_->index(rowIndex.row(), column).data().toString());
        }
        lines.push_back(values.join(u'\t'));
    }
    QApplication::clipboard()->setText(lines.join(u'\n'));
}

void MainWindow::explorerCopy()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString &path : paths)
        urls.push_back(QUrl::fromLocalFile(path));
    mime->setUrls(urls);
    mime->setText(paths.join(u'\n'));
    QApplication::clipboard()->setMimeData(mime);
}

void MainWindow::explorerCut()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnomeData("cut\n");
    for (const QString &path : paths) {
        const QUrl url = QUrl::fromLocalFile(path);
        urls.push_back(url);
        gnomeData += url.toEncoded() + '\n';
    }
    mime->setUrls(urls);
    mime->setText(paths.join(u'\n'));
    mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnomeData);
    QApplication::clipboard()->setMimeData(mime);
}

void MainWindow::startCurrentSearch()
{
    if (isSearching_ || options_.roots.trimmed().isEmpty())
        return;
    isSearching_ = true;
    isStopping_ = false;
    lastSearchOptions_ = options_;
    updateStaleSearchWarning();
    applyDisplayOptions();
    model_->setRows({});
    searchAction_->setEnabled(false);
    stopAction_->setEnabled(true);
    refreshAction_->setEnabled(false);
    progress_->show();
    progress_->setRange(0, 0);
    progress_->setFormat(tr("Scanning…"));
    setWindowTitle(tr("%1 — %2 — SearchMyFiles for Linux")
                       .arg(options_.roots, options_.mode));
    if (focusOnSearchStart_)
        table_->setFocus();
    emit startRequested(options_);
}

void MainWindow::searchFinished(const QVector<FileRecord> &results, const QString &message)
{
    isSearching_ = false;
    isStopping_ = false;
    model_->setRows(results);
    searchAction_->setEnabled(true);
    stopAction_->setEnabled(false);
    refreshAction_->setEnabled(true);
    progress_->hide();
    statusBar()->showMessage(message);
    updateStaleSearchWarning();
    if (autoSizeOnSearchEnd_) {
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table_->resizeColumnsToContents();
    }
    if (focusOnSearchEnd_)
        table_->setFocus();
}

void MainWindow::openSelected()
{
    for (const QString &path : selectedPaths())
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::openFolder()
{
    const QStringList paths = selectedPaths();
    if (!paths.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(paths.front()).absolutePath()));
}

// bug 5 fix: QProcess → QDesktopServices
void MainWindow::openWith()
{
    const QStringList paths = selectedPaths();
    if (!paths.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(paths.front()));
}

void MainWindow::selectInFileManager()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    QStringList urls;
    for (const QString &path : paths)
        urls.push_back(QUrl::fromLocalFile(path).toString());
    QDBusInterface fileManager(
        QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"),
        QDBusConnection::sessionBus());
    const QDBusMessage reply = fileManager.call(QStringLiteral("ShowItems"), urls, QString{});
    if (!fileManager.isValid() || reply.type() == QDBusMessage::ErrorMessage)
        openFolder();
}

void MainWindow::moveToTrash()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty()
        || QMessageBox::question(
               this, tr("Move To Trash"),
               tr("Move %1 selected item(s) to Trash?").arg(paths.size()),
               QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    int failed = 0;
    for (const QString &path : paths)
        if (!QFile::moveToTrash(path))
            ++failed;
    if (failed)
        QMessageBox::warning(this, tr("Move To Trash"),
                             tr("Failed to move %1 item(s) to Trash.").arg(failed));
    refreshSearch();
}

// bug 6 fix: QDir().rmdir → QDir(path).removeRecursively
void MainWindow::deleteSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty()
        || QMessageBox::warning(
               this, tr("Delete Selected Files"),
               tr("Permanently delete %1 selected item(s)?\n\nThis cannot be undone.")
                   .arg(paths.size()),
               QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    int failed = 0;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        const bool removed = info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
        if (!removed)
            ++failed;
    }
    if (failed)
        QMessageBox::warning(this, tr("Delete Selected Files"),
                             tr("Failed to delete %1 item(s).").arg(failed));
    refreshSearch();
}

void MainWindow::renameSelected()
{
    const auto record = currentRecord();
    if (!record)
        return;
    bool accepted = false;
    const QString newName = QInputDialog::getText(
        this, tr("Rename"), tr("New filename:"), QLineEdit::Normal,
        record->name, &accepted);
    if (!accepted || newName.isEmpty() || newName == record->name)
        return;
    const QString newPath = QDir(record->folder).filePath(newName);
    if (!QFile::rename(record->path, newPath))
        QMessageBox::critical(this, tr("Rename"), tr("Failed to rename this item."));
    else
        refreshSearch();
}

void MainWindow::showProperties()
{
    const auto record = currentRecord();
    if (!record)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Properties"));
    dialog.resize(780, 540);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    const auto addValue = [form](const QString &label, const QString &value) {
        auto *edit = new QLineEdit(value);
        edit->setReadOnly(true);
        edit->setCursorPosition(0);
        form->addRow(label, edit);
    };
    const auto date = [this](qint64 timestamp) {
        if (!timestamp)
            return QString{};
        QDateTime value = QDateTime::fromSecsSinceEpoch(timestamp);
        if (showGmt_)
            value = value.toUTC();
        return value.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    };
    addValue(tr("Filename:"), record->name);
    addValue(tr("Folder:"), record->folder);
    addValue(tr("Full Path:"), record->path);
    addValue(tr("Size:"), QString::number(record->size));
    addValue(tr("Size On Disk:"), QString::number(record->sizeOnDisk));
    addValue(tr("Modified Time:"), date(record->modified));
    addValue(tr("Created Time:"), date(record->created));
    addValue(tr("Last Accessed Time:"), date(record->accessed));
    addValue(tr("Entry Modified Time:"), date(record->changed));
    addValue(tr("Attributes:"), record->attributes);
    addValue(tr("Extension:"), QFileInfo(record->name).suffix());
    addValue(tr("Duplicate Set:"), record->group ? QString::number(record->group) : QString{});
    addValue(tr("Keeper Priority:"),
             record->duplicateCopy ? QString::number(record->duplicateCopy) : QString{});
    addValue(tr("File Owner:"), record->owner);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::copyPaths()
{
    QApplication::clipboard()->setText(selectedPaths().join(u'\n'));
}

void MainWindow::contextMenu(const QPoint &point)
{
    QMenu menu(this);
    menu.addAction(tr("Open Selected File"), QKeySequence(Qt::Key_F9), this, &MainWindow::openSelected);
    menu.addAction(tr("Open Folder"), QKeySequence(Qt::Key_F8), this, &MainWindow::openFolder);
    menu.addAction(tr("Open With…"), QKeySequence(Qt::Key_F7), this, &MainWindow::openWith);
    menu.addAction(tr("Select File In File Manager"), this, &MainWindow::selectInFileManager);
    menu.addSeparator();
    menu.addAction(tr("Explorer Copy"), QKeySequence(Qt::CTRL | Qt::Key_E),
                   this, &MainWindow::explorerCopy);
    menu.addAction(tr("Explorer Cut"), QKeySequence::Cut, this, &MainWindow::explorerCut);
    menu.addSeparator();
    menu.addAction(tr("Move To Trash"), QKeySequence(Qt::Key_Delete),
                   this, &MainWindow::moveToTrash);
    menu.addAction(tr("Delete Selected Files"), QKeySequence(Qt::SHIFT | Qt::Key_Delete),
                   this, &MainWindow::deleteSelected);
    menu.addAction(tr("Rename"), QKeySequence(Qt::CTRL | Qt::Key_R),
                   this, &MainWindow::renameSelected);
    menu.addSeparator();
    menu.addAction(tr("Save Files Information"), QKeySequence::Save,
                   this, &MainWindow::exportResults);
    menu.addAction(tr("Copy Files Information"), QKeySequence::Copy,
                   this, &MainWindow::copyInformation);
    menu.addSeparator();
    auto *columns = menu.addMenu(tr("Choose Columns"));
    for (int column = 0; column < ResultsModel::ColumnCount; ++column) {
        auto *action = columns->addAction(
            model_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
        action->setCheckable(true);
        action->setChecked(!table_->isColumnHidden(column));
        connect(action, &QAction::toggled, this, [this, column](bool visible) {
            table_->setColumnHidden(column, !visible);
            saveUiSettings();
        });
    }
    menu.addAction(tr("Auto Size Columns"), QKeySequence(Qt::CTRL | Qt::Key_Plus), this, [this] {
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table_->resizeColumnsToContents();
    });
    menu.addSeparator();
    menu.addAction(tr("Properties"), QKeySequence(Qt::ALT | Qt::Key_Return),
                   this, &MainWindow::showProperties);
    menu.addSeparator();
    menu.addAction(tr("Refresh"), QKeySequence(Qt::Key_F5), this, &MainWindow::refreshSearch);
    menu.exec(table_->mapToGlobal(point));
}

void MainWindow::headerContextMenu(const QPoint &point)
{
    QHeaderView *header = table_->horizontalHeader();
    const int clickedColumn = header->logicalIndexAt(point);
    QMenu menu(this);
    auto *autoWidthColumn = menu.addAction(tr("Auto Width This Column"));
    autoWidthColumn->setEnabled(clickedColumn >= 0);
    connect(autoWidthColumn, &QAction::triggered, this, [this, clickedColumn] {
        table_->horizontalHeader()->setSectionResizeMode(clickedColumn, QHeaderView::Interactive);
        table_->resizeColumnToContents(clickedColumn);
        saveUiSettings();
    });
    menu.addAction(tr("Auto Width All Columns"), this, [this] {
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table_->resizeColumnsToContents();
        saveUiSettings();
    });
    menu.addSeparator();
    auto *hideColumn = menu.addAction(tr("Hide This Column"));
    hideColumn->setEnabled(clickedColumn >= 0);
    connect(hideColumn, &QAction::triggered, this, [this, clickedColumn] {
        table_->setColumnHidden(clickedColumn, true);
        saveUiSettings();
    });
    auto *columns = menu.addMenu(tr("Choose Columns"));
    for (int column = 0; column < ResultsModel::ColumnCount; ++column) {
        auto *action = columns->addAction(
            model_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
        action->setCheckable(true);
        action->setChecked(!table_->isColumnHidden(column));
        connect(action, &QAction::toggled, this, [this, column](bool visible) {
            table_->setColumnHidden(column, !visible);
            saveUiSettings();
        });
    }
    menu.exec(header->mapToGlobal(point));
}

void MainWindow::exportResults()
{
    QVector<FileRecord> rows;
    const QStringList selectedList = selectedPaths();
    const QSet<QString> selected(selectedList.cbegin(), selectedList.cend());
    for (const auto &record : model_->rows())
        if (selected.isEmpty() || selected.contains(record.path))
            rows.push_back(record);
    if (rows.isEmpty())
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export results"), QStringLiteral("search-results.csv"),
        tr("CSV (*.csv);;Text (*.txt *.tsv);;HTML (*.html);;XML (*.xml);;JSON (*.json)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("Export failed"), file.errorString());
        return;
    }
    const auto quoted = [](QString value) {
        value.replace(u'"', QStringLiteral("\"\""));
        return u'"' + value + u'"';
    };
    const auto values = [](const FileRecord &r) {
        const auto date = [](qint64 t) { return t ? QDateTime::fromSecsSinceEpoch(t).toString(Qt::ISODate) : QString{}; };
        return QStringList{
            r.name, r.folder, QString::number(r.size), QString::number(r.sizeOnDisk),
            date(r.modified), date(r.created), date(r.accessed), date(r.changed),
            r.attributes, QFileInfo(r.name).suffix(),
            r.group ? QString::number(r.group) : QString{},
            r.duplicateCopy ? QString::number(r.duplicateCopy) : QString{},
            r.type, r.owner, r.path
        };
    };
    const QStringList headers{
        tr("Filename"), tr("Folder"), tr("Size"), tr("Size On Disk"), tr("Modified Time"),
        tr("Created Time"), tr("Last Accessed Time"), tr("Entry Modified Time"),
        tr("Attributes"), tr("Extension"), tr("Duplicate Set"), tr("Keeper Priority"),
        tr("Type"), tr("File Owner"), tr("Full Path")
    };
    QTextStream out(&file);
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("json")) {
        QJsonArray array;
        for (const auto &record : rows) {
            QJsonObject object;
            const QStringList row = values(record);
            for (int i = 0; i < headers.size(); ++i)
                object.insert(headers[i], row[i]);
            array.append(object);
        }
        file.write(QJsonDocument(array).toJson());
    } else if (suffix == QStringLiteral("html")) {
        out << "<!doctype html><meta charset=\"utf-8\"><table><thead><tr>";
        for (const auto &header : headers) out << "<th>" << header.toHtmlEscaped() << "</th>";
        out << "</tr></thead><tbody>";
        for (const auto &record : rows) {
            out << "<tr>";
            for (const auto &value : values(record)) out << "<td>" << value.toHtmlEscaped() << "</td>";
            out << "</tr>";
        }
        out << "</tbody></table>";
    } else if (suffix == QStringLiteral("xml")) {
        out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?><SearchResults>";
        for (const auto &record : rows) {
            out << "<File>";
            const auto row = values(record);
            for (int i = 0; i < headers.size(); ++i) {
                QString tag = headers[i];
                tag.remove(u' ');
                out << "<" << tag << ">" << row[i].toHtmlEscaped()
                    << "</" << tag << ">";
            }
            out << "</File>";
        }
        out << "</SearchResults>";
    } else {
        const QChar separator = suffix == QStringLiteral("csv") ? u',' : u'\t';
        if (addExportHeader_)
            out << headers.join(separator) << u'\n';
        for (const auto &record : rows) {
            QStringList row = values(record);
            if (separator == u',')
                for (QString &value : row) value = quoted(value);
            out << row.join(separator) << u'\n';
        }
    }
    statusBar()->showMessage(tr("Exported %1 results to %2").arg(rows.size()).arg(path), 5000);
}

std::optional<FileRecord> MainWindow::currentRecord() const
{
    const QModelIndex current = table_->currentIndex();
    if (!current.isValid())
        return std::nullopt;
    const int row = proxy_->mapToSource(current).row();
    if (row < 0 || row >= model_->rows().size())
        return std::nullopt;
    return model_->rows().at(row);
}

QStringList MainWindow::actionChoices() const
{
    return {tr("No Action"), tr("Open Properties Window"), tr("Open Selected File"),
            tr("Open With…"), tr("Open Folder"), tr("Select File In File Manager")};
}

void MainWindow::executeConfiguredAction(const QString &action)
{
    if (action == tr("Open Properties Window"))
        showProperties();
    else if (action == tr("Open Selected File"))
        openSelected();
    else if (action == tr("Open With…"))
        openWith();
    else if (action == tr("Open Folder"))
        openFolder();
    else if (action == tr("Select File In File Manager"))
        selectInFileManager();
}

QString MainWindow::sizeUnitName() const
{
    static const QStringList units{tr("Automatic"), tr("Bytes"), tr("KB"), tr("MB"), tr("GB")};
    return units.value(sizeUnit_, units.front());
}

int MainWindow::sizeUnitFromName(const QString &name) const
{
    const QStringList units{tr("Automatic"), tr("Bytes"), tr("KB"), tr("MB"), tr("GB")};
    return std::max(0, int(units.indexOf(name)));
}

void MainWindow::applyDisplayOptions()
{
    const int unit = options_.mode == QStringLiteral("Summary")
        ? sizeUnitFromName(summarySizeUnitName_) : sizeUnit_;
    model_->setDisplayOptions(unit, showGmt_, markDuplicates_, duplicateColorSet_);
}

void MainWindow::saveUiSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/sizeUnit"), sizeUnit_);
    settings.setValue(QStringLiteral("ui/summarySizeUnit"), summarySizeUnitName_);
    settings.setValue(QStringLiteral("ui/showGmt"), showGmt_);
    settings.setValue(QStringLiteral("ui/markDuplicates"), markDuplicates_);
    settings.setValue(QStringLiteral("ui/duplicateColorSet"), duplicateColorSet_);
    settings.setValue(QStringLiteral("ui/doubleClickAction"), doubleClickAction_);
    settings.setValue(QStringLiteral("ui/enterKeyAction"), enterKeyAction_);
    settings.setValue(QStringLiteral("ui/addExportHeader"), addExportHeader_);
    settings.setValue(QStringLiteral("ui/focusOnSearchStart"), focusOnSearchStart_);
    settings.setValue(QStringLiteral("ui/focusOnSearchEnd"), focusOnSearchEnd_);
    settings.setValue(QStringLiteral("ui/autoSizeOnSearchEnd"), autoSizeOnSearchEnd_);
    settings.setValue(QStringLiteral("ui/showGrid"), showGrid_);
    settings.setValue(QStringLiteral("ui/showTooltips"), showTooltips_);
    settings.setValue(QStringLiteral("ui/windowGeometry"), saveGeometry());
    if (table_)
        settings.setValue(QStringLiteral("ui/headerState"), table_->horizontalHeader()->saveState());
}

QStringList MainWindow::selectedPaths() const
{
    QStringList paths;
    QModelIndexList selected = table_->selectionModel()->selectedRows();
    std::sort(selected.begin(), selected.end(),
              [](const QModelIndex &left, const QModelIndex &right) {
        return left.row() < right.row();
    });
    for (const QModelIndex &index : selected) {
        const int sourceRow = proxy_->mapToSource(index).row();
        paths.push_back(model_->data(
            model_->index(sourceRow, ResultsModel::Name), Qt::UserRole).toString());
    }
    return paths;
}

void MainWindow::saveOptions()
{
    QSettings settings;
    settings.setValue(QStringLiteral("roots"), options_.roots);
    settings.setValue(QStringLiteral("fileWildcards"), options_.fileWildcards);
    settings.setValue(QStringLiteral("subfolderWildcards"), options_.subfolderWildcards);
    settings.setValue(QStringLiteral("excludeFiles"), options_.excludeFiles);
    settings.setValue(QStringLiteral("excludeFolders"), options_.excludeFolders);
    settings.setValue(QStringLiteral("includeFolders"), options_.includeFolders);
    settings.setValue(QStringLiteral("contains"), options_.contains);
    settings.setValue(QStringLiteral("binary"), options_.binary);
    settings.setValue(QStringLiteral("multipleValues"), options_.multipleValues);
    settings.setValue(QStringLiteral("multipleAnd"), options_.multipleAnd);
    settings.setValue(QStringLiteral("caseSensitive"), options_.caseSensitive);
    settings.setValue(QStringLiteral("minSize"), options_.minSize);
    settings.setValue(QStringLiteral("maxSize"), options_.maxSize);
    settings.setValue(QStringLiteral("timeField"), options_.timeField);
    settings.setValue(QStringLiteral("lastMinutes"), options_.lastMinutes);
    settings.setValue(QStringLiteral("recursive"), options_.recursive);
    settings.setValue(QStringLiteral("findFiles"), options_.findFiles);
    settings.setValue(QStringLiteral("findFolders"), options_.findFolders);
    settings.setValue(QStringLiteral("followLinks"), options_.followLinks);
    settings.setValue(QStringLiteral("retrieveOwner"), options_.retrieveOwner);
    settings.setValue(QStringLiteral("accurateProgress"), options_.accurateProgress);
    settings.setValue(QStringLiteral("useCache"), options_.useCache);
    settings.setValue(QStringLiteral("maxDepth"), options_.maxDepth);
    settings.setValue(QStringLiteral("maxResults"), options_.maxResults);
    settings.setValue(QStringLiteral("mode"), options_.mode);
    settings.setValue(QStringLiteral("duplicateNameMode"), options_.duplicateNameMode);
    settings.setValue(QStringLiteral("duplicateNameWithoutExtension"), options_.duplicateNameWithoutExtension);
    settings.setValue(QStringLiteral("strictDuplicateComparison"),
                      options_.strictDuplicateComparison);
    settings.setValue(QStringLiteral("showDuplicateCopiesOnly"), options_.showDuplicateCopiesOnly);
    settings.setValue(QStringLiteral("includeSubfoldersInSummary"), options_.includeSubfoldersInSummary);
    settings.setValue(QStringLiteral("hidden"), options_.hidden);
    settings.setValue(QStringLiteral("readonly"), options_.readonly);
    settings.setValue(QStringLiteral("executable"), options_.executable);
    updateStaleSearchWarning();
}

void MainWindow::updateStaleSearchWarning()
{
    if (!staleSearchWarning_)
        return;
    staleSearchWarning_->setVisible(
        lastSearchOptions_.has_value() && options_ != *lastSearchOptions_);
}

void MainWindow::loadOptions()
{
    QSettings s;
    options_.roots = s.value(QStringLiteral("roots"), options_.roots).toString();
    options_.fileWildcards = s.value(QStringLiteral("fileWildcards"), options_.fileWildcards).toString();
    options_.subfolderWildcards = s.value(QStringLiteral("subfolderWildcards"), options_.subfolderWildcards).toString();
    options_.excludeFiles = s.value(QStringLiteral("excludeFiles")).toString();
    options_.excludeFolders = s.value(QStringLiteral("excludeFolders")).toString();
    options_.includeFolders = s.value(QStringLiteral("includeFolders")).toString();
    options_.contains = s.value(QStringLiteral("contains")).toString();
    options_.binary = s.value(QStringLiteral("binary"), false).toBool();
    options_.multipleValues = s.value(QStringLiteral("multipleValues"), false).toBool();
    options_.multipleAnd = s.value(QStringLiteral("multipleAnd"), false).toBool();
    options_.caseSensitive = s.value(QStringLiteral("caseSensitive"), false).toBool();
    options_.minSize = s.value(QStringLiteral("minSize"), 0).toLongLong();
    options_.maxSize = s.value(QStringLiteral("maxSize"), 0).toLongLong();
    options_.timeField = s.value(QStringLiteral("timeField"), options_.timeField).toString();
    options_.lastMinutes = s.value(QStringLiteral("lastMinutes"), 0).toInt();
    options_.recursive = s.value(QStringLiteral("recursive"), true).toBool();
    options_.findFiles = s.value(QStringLiteral("findFiles"), true).toBool();
    options_.findFolders = s.value(QStringLiteral("findFolders"), false).toBool();
    options_.followLinks = s.value(QStringLiteral("followLinks"), false).toBool();
    options_.retrieveOwner = s.value(QStringLiteral("retrieveOwner"), false).toBool();
    options_.accurateProgress = s.value(QStringLiteral("accurateProgress"), true).toBool();
    options_.useCache = s.value(QStringLiteral("useCache"), true).toBool();
    options_.maxDepth = s.value(QStringLiteral("maxDepth"), 0).toInt();
    options_.maxResults = s.value(QStringLiteral("maxResults"), 0).toInt();
    options_.mode = s.value(QStringLiteral("mode"), options_.mode).toString();
    options_.duplicateNameMode = s.value(QStringLiteral("duplicateNameMode"), options_.duplicateNameMode).toString();
    options_.duplicateNameWithoutExtension = s.value(QStringLiteral("duplicateNameWithoutExtension"), false).toBool();
    options_.strictDuplicateComparison =
        s.value(QStringLiteral("strictDuplicateComparison"), false).toBool();
    options_.showDuplicateCopiesOnly =
        s.value(QStringLiteral("showDuplicateCopiesOnly"),
                s.value(QStringLiteral("showOnlyDuplicateFiles"), true)).toBool();
    options_.includeSubfoldersInSummary = s.value(QStringLiteral("includeSubfoldersInSummary"), false).toBool();
    options_.hidden = s.value(QStringLiteral("hidden"), options_.hidden).toString();
    options_.readonly = s.value(QStringLiteral("readonly"), options_.readonly).toString();
    options_.executable = s.value(QStringLiteral("executable"), options_.executable).toString();
    sizeUnit_ = s.value(QStringLiteral("ui/sizeUnit"), 0).toInt();
    summarySizeUnitName_ = s.value(QStringLiteral("ui/summarySizeUnit"), tr("Automatic")).toString();
    showGmt_ = s.value(QStringLiteral("ui/showGmt"), false).toBool();
    markDuplicates_ = s.value(QStringLiteral("ui/markDuplicates"), true).toBool();
    duplicateColorSet_ = s.value(QStringLiteral("ui/duplicateColorSet"), 1).toInt();
    doubleClickAction_ = s.value(
        QStringLiteral("ui/doubleClickAction"), tr("Open Properties Window")).toString();
    enterKeyAction_ = s.value(
        QStringLiteral("ui/enterKeyAction"), tr("Open Selected File")).toString();
    addExportHeader_ = s.value(QStringLiteral("ui/addExportHeader"), true).toBool();
    focusOnSearchStart_ = s.value(QStringLiteral("ui/focusOnSearchStart"), true).toBool();
    focusOnSearchEnd_ = s.value(QStringLiteral("ui/focusOnSearchEnd"), true).toBool();
    autoSizeOnSearchEnd_ = s.value(QStringLiteral("ui/autoSizeOnSearchEnd"), false).toBool();
    showGrid_ = s.value(QStringLiteral("ui/showGrid"), true).toBool();
    showTooltips_ = s.value(QStringLiteral("ui/showTooltips"), true).toBool();
}
