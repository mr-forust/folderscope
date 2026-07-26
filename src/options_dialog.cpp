#include "options_dialog.h"

#include <QDialogButtonBox>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>

#include "long_long_spin_box.h"

OptionsDialog::OptionsDialog(const SearchOptions &o, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Search Options"));
    resize(760, 560);
    setupUi(o);
}

void OptionsDialog::setupUi(const SearchOptions &o)
{
    auto *layout = new QVBoxLayout(this);

    auto *profileRow = new QWidget;
    auto *profileLayout = new QHBoxLayout(profileRow);
    profileLayout->setContentsMargins(0, 0, 0, 0);
    profileCombo_ = new QComboBox;
    profileCombo_->setEditable(false);
    profileCombo_->addItem(tr("(default)"));
    profileCombo_->setMinimumWidth(200);
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("profiles"));
        const QStringList profiles = settings.childGroups();
        for (const QString &name : profiles)
            profileCombo_->addItem(name);
        settings.endGroup();
    }
    auto *saveBtn = new QPushButton(tr("Save Profile…"));
    auto *loadBtn = new QPushButton(tr("Load Profile"));
    auto *deleteBtn = new QPushButton(tr("Delete Profile"));
    profileLayout->addWidget(new QLabel(tr("Profile:")));
    profileLayout->addWidget(profileCombo_);
    profileLayout->addWidget(saveBtn);
    profileLayout->addWidget(loadBtn);
    profileLayout->addWidget(deleteBtn);
    layout->addWidget(profileRow);

    connect(saveBtn, &QPushButton::clicked, this, &OptionsDialog::saveProfile);
    connect(loadBtn, &QPushButton::clicked, this, &OptionsDialog::loadProfile);
    connect(deleteBtn, &QPushButton::clicked, this, [this] {
        const QString name = profileCombo_->currentText();
        if (name.isEmpty() || name == tr("(default)"))
            return;
        if (QMessageBox::question(this, tr("Delete Profile"),
                tr("Delete profile \"%1\"?").arg(name))
                != QMessageBox::Yes)
            return;
        QSettings settings;
        settings.beginGroup(QStringLiteral("profiles"));
        settings.remove(name);
        settings.endGroup();
        profileCombo_->removeItem(profileCombo_->currentIndex());
    });

    auto *modeGroup = new QGroupBox(tr("Search mode"));
    auto *modeLayout = new QHBoxLayout(modeGroup);
    mode_ = new QComboBox;
    mode_->addItems({tr("Standard search"), tr("Duplicates search"), tr("Non-Duplicates search"),
                     tr("Summary"), tr("Duplicate names search")});
    mode_->setCurrentText(o.mode);
    duplicateMode_ = new QComboBox;
    duplicateMode_->addItems({tr("All files and folders"), tr("Only files"),
                              tr("Only identical content"), tr("Only non-identical content")});
    duplicateMode_->setCurrentText(o.duplicateNameMode);
    duplicateWithoutExtension_ = new QCheckBox(tr("Compare names without extension"));
    duplicateWithoutExtension_->setChecked(o.duplicateNameWithoutExtension);
    modeLayout->addWidget(mode_);
    modeLayout->addWidget(new QLabel(tr("Duplicate-name mode:")));
    modeLayout->addWidget(duplicateMode_);
    modeLayout->addWidget(duplicateWithoutExtension_);
    layout->addWidget(modeGroup);

    auto *tabs = new QTabWidget;
    layout->addWidget(tabs);

    auto *filesPage = new QWidget;
    auto *filesForm = new QFormLayout(filesPage);
    roots_ = new QLineEdit(o.roots);
    roots_->setAcceptDrops(true);
    roots_->setToolTip(tr("Drop folders here from your file manager"));
    roots_->installEventFilter(this);
    auto *rootRow = new QWidget;
    auto *rootLayout = new QHBoxLayout(rootRow);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->addWidget(roots_);
    auto *browse = new QPushButton(tr("Browse…"));
    rootLayout->addWidget(browse);
    filesForm->addRow(tr("Base folders (separate with ;):"), rootRow);
    fileMasks_ = addLine(filesForm, tr("Files wildcard:"), o.fileWildcards);
    subfolderMasks_ = addLine(filesForm, tr("Subfolders wildcard:"), o.subfolderWildcards);
    excludeFiles_ = addLine(filesForm, tr("Exclude files:"), o.excludeFiles);
    excludeFolders_ = addLine(filesForm, tr("Exclude folders:"), o.excludeFolders);
    includeFolders_ = addLine(filesForm, tr("Include only folders:"), o.includeFolders);
    recursive_ = new QCheckBox(tr("Search subfolders")); recursive_->setChecked(o.recursive);
    findFiles_ = new QCheckBox(tr("Find files")); findFiles_->setChecked(o.findFiles);
    findFolders_ = new QCheckBox(tr("Find folders")); findFolders_->setChecked(o.findFolders);
    followLinks_ = new QCheckBox(tr("Follow symbolic links")); followLinks_->setChecked(o.followLinks);
    retrieveOwner_ = new QCheckBox(tr("Retrieve file owner (slower)")); retrieveOwner_->setChecked(o.retrieveOwner);
    accurateProgress_ = new QCheckBox(tr("Count files first for accurate progress"));
    accurateProgress_->setChecked(o.accurateProgress);
    useCache_ = new QCheckBox(tr("Use persistent cache for hashes and content searches"));
    useCache_->setChecked(o.useCache);
    strictDuplicateComparison_ =
        new QCheckBox(tr("Strict duplicate comparison (byte by byte)"));
    strictDuplicateComparison_->setObjectName(
        QStringLiteral("strictDuplicateComparison"));
    strictDuplicateComparison_->setChecked(o.strictDuplicateComparison);
    maxDepth_ = new QSpinBox; maxDepth_->setRange(0, 1024); maxDepth_->setValue(o.maxDepth);
    maxDepth_->setSpecialValueText(tr("Unlimited"));
    maxResults_ = new QSpinBox; maxResults_->setRange(0, 100'000'000); maxResults_->setValue(o.maxResults);
    maxResults_->setSpecialValueText(tr("Unlimited"));
    filesForm->addRow(recursive_);
    filesForm->addRow(tr("Subfolders depth:"), maxDepth_);
    filesForm->addRow(findFiles_);
    filesForm->addRow(findFolders_);
    filesForm->addRow(followLinks_);
    filesForm->addRow(retrieveOwner_);
    filesForm->addRow(accurateProgress_);
    filesForm->addRow(useCache_);
    filesForm->addRow(strictDuplicateComparison_);
    filesForm->addRow(tr("Stop after finding:"), maxResults_);
    tabs->addTab(filesPage, tr("Files & folders"));

    auto *contentPage = new QWidget;
    auto *contentForm = new QFormLayout(contentPage);
    contains_ = new QTextEdit(o.contains);
    contains_->setMaximumHeight(110);
    contentForm->addRow(tr("File contains:"), contains_);
    binary_ = new QCheckBox(tr("Binary search (hex bytes, e.g. A2 C5 2F)")); binary_->setChecked(o.binary);
    multiple_ = new QCheckBox(tr("Search multiple values (comma separated)")); multiple_->setChecked(o.multipleValues);
    multipleMode_ = new QComboBox; multipleMode_->addItems({tr("Or"), tr("And")});
    multipleMode_->setCurrentIndex(o.multipleAnd ? 1 : 0);
    caseSensitive_ = new QCheckBox(tr("Case sensitive")); caseSensitive_->setChecked(o.caseSensitive);
    contentForm->addRow(binary_);
    contentForm->addRow(multiple_);
    contentForm->addRow(tr("Multiple values match:"), multipleMode_);
    contentForm->addRow(caseSensitive_);
    tabs->addTab(contentPage, tr("File contents"));

    auto *filterPage = new QWidget;
    auto *filterForm = new QFormLayout(filterPage);
    minSize_ = sizeSpin(o.minSize);
    maxSize_ = sizeSpin(o.maxSize);
    maxSize_->setSuffix(tr(" bytes (0 = any)"));
    filterForm->addRow(tr("Minimum size:"), minSize_);
    filterForm->addRow(tr("Maximum size:"), maxSize_);
    timeField_ = new QComboBox;
    timeField_->addItems({tr("Modified time"), tr("Accessed time"), tr("Metadata changed time")});
    timeField_->setCurrentText(o.timeField);
    filterForm->addRow(tr("Time field:"), timeField_);
    lastMinutes_ = new QSpinBox;
    lastMinutes_->setRange(0, 10'000'000);
    lastMinutes_->setSuffix(tr(" minutes (0 = any)"));
    lastMinutes_->setValue(o.lastMinutes);
    filterForm->addRow(tr("Within last:"), lastMinutes_);
    minTime_ = new QLineEdit(o.minTime ? QDateTime::fromSecsSinceEpoch(o.minTime).toString(Qt::ISODate) : QString{});
    maxTime_ = new QLineEdit(o.maxTime ? QDateTime::fromSecsSinceEpoch(o.maxTime).toString(Qt::ISODate) : QString{});
    minTime_->setPlaceholderText(tr("YYYY-MM-DDTHH:MM:SS"));
    maxTime_->setPlaceholderText(tr("YYYY-MM-DDTHH:MM:SS"));
    filterForm->addRow(tr("From time:"), minTime_);
    filterForm->addRow(tr("To time:"), maxTime_);
    hidden_ = attrCombo(o.hidden);
    readonly_ = attrCombo(o.readonly);
    executable_ = attrCombo(o.executable);
    filterForm->addRow(tr("Hidden:"), hidden_);
    filterForm->addRow(tr("Read-only:"), readonly_);
    filterForm->addRow(tr("Executable:"), executable_);
    tabs->addTab(filterPage, tr("Size, time & attributes"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &OptionsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QStringList existingFolders = patterns(roots_->text());
        const QString startFolder = existingFolders.isEmpty()
            ? QDir::homePath() : existingFolders.constLast();
        const QString folder = QFileDialog::getExistingDirectory(
            this, tr("Choose base folder"), startFolder);
        if (folder.isEmpty())
            return;

        QStringList folders = existingFolders;
        const QString cleanPath = QDir::cleanPath(folder);
        if (!folders.contains(cleanPath))
            folders.push_back(cleanPath);
        roots_->setText(folders.join(u';'));
    });
    layout->addWidget(buttons);
}

SearchOptions OptionsDialog::options() const
{
    SearchOptions o;
    o.roots = roots_->text();
    o.fileWildcards = fileMasks_->text();
    o.subfolderWildcards = subfolderMasks_->text();
    o.excludeFiles = excludeFiles_->text();
    o.excludeFolders = excludeFolders_->text();
    o.includeFolders = includeFolders_->text();
    o.contains = contains_->toPlainText();
    o.binary = binary_->isChecked();
    o.multipleValues = multiple_->isChecked();
    o.multipleAnd = multipleMode_->currentIndex() == 1;
    o.caseSensitive = caseSensitive_->isChecked();
    o.minSize = minSize_->value();
    o.maxSize = maxSize_->value();
    o.timeField = timeField_->currentText();
    o.lastMinutes = lastMinutes_->value();
    o.minTime = dateValue(minTime_);
    o.maxTime = dateValue(maxTime_);
    o.recursive = recursive_->isChecked();
    o.findFiles = findFiles_->isChecked();
    o.findFolders = findFolders_->isChecked();
    o.followLinks = followLinks_->isChecked();
    o.retrieveOwner = retrieveOwner_->isChecked();
    o.accurateProgress = accurateProgress_->isChecked();
    o.useCache = useCache_->isChecked();
    o.strictDuplicateComparison = strictDuplicateComparison_->isChecked();
    o.maxDepth = maxDepth_->value();
    o.maxResults = maxResults_->value();
    o.mode = mode_->currentText();
    o.duplicateNameMode = duplicateMode_->currentText();
    o.duplicateNameWithoutExtension = duplicateWithoutExtension_->isChecked();
    o.hidden = hidden_->currentText();
    o.readonly = readonly_->currentText();
    o.executable = executable_->currentText();
    return o;
}

void OptionsDialog::accept()
{
    if (!validateDate(minTime_, tr("From time"))
        || !validateDate(maxTime_, tr("To time")))
        return;
    QDialog::accept();
}

QLineEdit *OptionsDialog::addLine(QFormLayout *layout, const QString &label, const QString &value)
{
    auto *edit = new QLineEdit(value);
    layout->addRow(label, edit);
    return edit;
}

LongLongSpinBox *OptionsDialog::sizeSpin(qint64 value)
{
    auto *spin = new LongLongSpinBox;
    spin->setRange(0, std::numeric_limits<qint64>::max());
    spin->setSuffix(QObject::tr(" bytes"));
    spin->setValue(value);
    return spin;
}

qint64 OptionsDialog::dateValue(const QLineEdit *edit)
{
    qint64 seconds = 0;
    parseOptionalIsoDate(edit->text(), seconds);
    return seconds;
}

bool OptionsDialog::validateDate(QLineEdit *edit, const QString &label)
{
    qint64 seconds = 0;
    if (parseOptionalIsoDate(edit->text(), seconds))
        return true;
    QMessageBox::warning(
        this, tr("Invalid date"),
        tr("%1 must be a valid ISO date and time, for example:\n2026-07-25T20:30:00")
            .arg(label));
    edit->setFocus();
    edit->selectAll();
    return false;
}

QComboBox *OptionsDialog::attrCombo(const QString &value)
{
    auto *combo = new QComboBox;
    combo->addItems({QObject::tr("Any"), QObject::tr("Yes"), QObject::tr("No")});
    combo->setCurrentText(value);
    return combo;
}

bool OptionsDialog::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == roots_ && event->type() == QEvent::DragEnter) {
        auto *dragEnter = static_cast<QDragEnterEvent *>(event);
        if (dragEnter->mimeData()->hasUrls()) {
            dragEnter->acceptProposedAction();
            return true;
        }
        return false;
    }
    if (obj == roots_ && event->type() == QEvent::Drop) {
        auto *drop = static_cast<QDropEvent *>(event);
        if (!drop->mimeData()->hasUrls())
            return false;
        QStringList folders = patterns(roots_->text());
        for (const QUrl &url : drop->mimeData()->urls()) {
            if (!url.isLocalFile())
                continue;
            const QString cleanPath = QDir::cleanPath(url.toLocalFile());
            if (QFileInfo::exists(cleanPath) && !folders.contains(cleanPath))
                folders.append(cleanPath);
        }
        roots_->setText(folders.join(u';'));
        drop->acceptProposedAction();
        return true;
    }
    return QDialog::eventFilter(obj, event);
}

void OptionsDialog::saveProfile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Save Profile"), tr("Profile name:"), QLineEdit::Normal, QString{}, &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    const QString profileName = name.trimmed();
    const SearchOptions opt = options();

    QSettings settings;
    settings.beginGroup(QStringLiteral("profiles"));
    settings.beginGroup(profileName);

    auto save = [&](const char *key, const QString &val) { settings.setValue(QString::fromLatin1(key), val); };
    auto saveBool = [&](const char *key, bool val) { settings.setValue(QString::fromLatin1(key), val); };
    auto saveInt = [&](const char *key, int val) { settings.setValue(QString::fromLatin1(key), val); };
    auto saveLong = [&](const char *key, qint64 val) { settings.setValue(QString::fromLatin1(key), val); };

    save("roots", opt.roots);
    save("fileWildcards", opt.fileWildcards);
    save("subfolderWildcards", opt.subfolderWildcards);
    save("excludeFiles", opt.excludeFiles);
    save("excludeFolders", opt.excludeFolders);
    save("includeFolders", opt.includeFolders);
    save("contains", opt.contains);
    save("mode", opt.mode);
    save("duplicateNameMode", opt.duplicateNameMode);
    save("timeField", opt.timeField);
    save("hidden", opt.hidden);
    save("readonly", opt.readonly);
    save("executable", opt.executable);

    saveBool("binary", opt.binary);
    saveBool("multipleValues", opt.multipleValues);
    saveBool("multipleAnd", opt.multipleAnd);
    saveBool("caseSensitive", opt.caseSensitive);
    saveBool("recursive", opt.recursive);
    saveBool("findFiles", opt.findFiles);
    saveBool("findFolders", opt.findFolders);
    saveBool("followLinks", opt.followLinks);
    saveBool("retrieveOwner", opt.retrieveOwner);
    saveBool("accurateProgress", opt.accurateProgress);
    saveBool("useCache", opt.useCache);
    saveBool("duplicateNameWithoutExtension", opt.duplicateNameWithoutExtension);
    saveBool("strictDuplicateComparison", opt.strictDuplicateComparison);

    saveInt("lastMinutes", opt.lastMinutes);
    saveInt("maxDepth", opt.maxDepth);
    saveInt("maxResults", opt.maxResults);

    saveLong("minSize", opt.minSize);
    saveLong("maxSize", opt.maxSize);
    saveLong("minTime", opt.minTime);
    saveLong("maxTime", opt.maxTime);

    settings.endGroup();
    settings.endGroup();

    if (profileCombo_->findText(profileName) < 0)
        profileCombo_->addItem(profileName);
    profileCombo_->setCurrentText(profileName);
}

void OptionsDialog::loadProfile()
{
    const QString name = profileCombo_->currentText();
    if (name.isEmpty() || name == tr("(default)"))
        return;

    QSettings settings;
    settings.beginGroup(QStringLiteral("profiles"));
    settings.beginGroup(name);

    auto load = [&](const char *key) { return settings.value(QString::fromLatin1(key)).toString(); };
    auto loadBool = [&](const char *key, bool def) { return settings.value(QString::fromLatin1(key), def).toBool(); };
    auto loadInt = [&](const char *key, int def) { return settings.value(QString::fromLatin1(key), def).toInt(); };
    auto loadLong = [&](const char *key, qint64 def) { return settings.value(QString::fromLatin1(key), def).toLongLong(); };

    roots_->setText(load("roots"));
    fileMasks_->setText(load("fileWildcards"));
    subfolderMasks_->setText(load("subfolderWildcards"));
    excludeFiles_->setText(load("excludeFiles"));
    excludeFolders_->setText(load("excludeFolders"));
    includeFolders_->setText(load("includeFolders"));
    contains_->setText(load("contains"));
    binary_->setChecked(loadBool("binary", false));
    multiple_->setChecked(loadBool("multipleValues", false));
    multipleMode_->setCurrentIndex(loadBool("multipleAnd", false) ? 1 : 0);
    caseSensitive_->setChecked(loadBool("caseSensitive", false));
    minSize_->setValue(loadLong("minSize", 0));
    maxSize_->setValue(loadLong("maxSize", 0));
    timeField_->setCurrentText(load("timeField"));
    lastMinutes_->setValue(loadInt("lastMinutes", 0));
    minTime_->setText(loadLong("minTime", 0)
        ? QDateTime::fromSecsSinceEpoch(loadLong("minTime", 0)).toString(Qt::ISODate) : QString{});
    maxTime_->setText(loadLong("maxTime", 0)
        ? QDateTime::fromSecsSinceEpoch(loadLong("maxTime", 0)).toString(Qt::ISODate) : QString{});
    recursive_->setChecked(loadBool("recursive", true));
    findFiles_->setChecked(loadBool("findFiles", true));
    findFolders_->setChecked(loadBool("findFolders", false));
    followLinks_->setChecked(loadBool("followLinks", false));
    retrieveOwner_->setChecked(loadBool("retrieveOwner", false));
    accurateProgress_->setChecked(loadBool("accurateProgress", true));
    useCache_->setChecked(loadBool("useCache", true));
    strictDuplicateComparison_->setChecked(
        loadBool("strictDuplicateComparison", false));
    maxDepth_->setValue(loadInt("maxDepth", 0));
    maxResults_->setValue(loadInt("maxResults", 0));
    mode_->setCurrentText(load("mode"));
    duplicateMode_->setCurrentText(load("duplicateNameMode"));
    duplicateWithoutExtension_->setChecked(loadBool("duplicateNameWithoutExtension", false));
    hidden_->setCurrentText(load("hidden"));
    readonly_->setCurrentText(load("readonly"));
    executable_->setCurrentText(load("executable"));

    settings.endGroup();
    settings.endGroup();
}
