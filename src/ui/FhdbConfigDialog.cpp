#include "FhdbConfigDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <set>

FhdbConfigDialog::FhdbConfigDialog(QWidget *parent) : QDialog(parent),
    config(Ps2::FhdbConfig::CreateDefault())
{
    setWindowTitle("FHDB Menu Configuration");
    resize(1120, 680);
    setMinimumSize(860, 540);

    auto *layout = new QVBoxLayout(this);
    auto *fileBar = new QHBoxLayout();
    auto *newButton = new QPushButton("New Default", this);
    auto *openButton = new QPushButton("Open FREEHDB.CNF...", this);
    auto *saveButton = new QPushButton("Save FREEHDB.CNF As...", this);
    pathLabel = new QLabel(this);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fileBar->addWidget(newButton);
    fileBar->addWidget(openButton);
    fileBar->addWidget(saveButton);
    fileBar->addSpacing(12);
    fileBar->addWidget(pathLabel, 1);
    layout->addLayout(fileBar);

    auto *bootGroup = new QGroupBox("Boot behaviour", this);
    auto *bootLayout = new QFormLayout(bootGroup);
    fastBoot = new QCheckBox("Enable FastBoot", bootGroup);
    bootLayout->addRow(QString(), fastBoot);
    defaultAction = new QLineEdit(bootGroup);
    defaultAction->setToolTip("Usually OSDSYS, FASTBOOT, or a full mass:/, mc?:/ or hdd0: path");
    bootLayout->addRow("Default action:", defaultAction);
    displayedItems = new QSpinBox(bootGroup);
    displayedItems->setRange(1, 100);
    bootLayout->addRow("Visible menu rows:", displayedItems);
    layout->addWidget(bootGroup);

    menuTable = new QTableWidget(this);
    menuTable->setColumnCount(5);
    menuTable->setHorizontalHeaderLabels({ "Item #", "Menu name", "Primary path",
            "Fallback path 2", "Fallback path 3" });
    menuTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    menuTable->setSelectionMode(QAbstractItemView::SingleSelection);
    menuTable->verticalHeader()->setVisible(false);
    menuTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    menuTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    menuTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    menuTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    menuTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    layout->addWidget(menuTable, 1);

    auto *itemBar = new QHBoxLayout();
    auto *addButton = new QPushButton("Add Item", this);
    auto *removeButton = new QPushButton("Remove Item", this);
    auto *upButton = new QPushButton("Move Up", this);
    auto *downButton = new QPushButton("Move Down", this);
    itemBar->addWidget(addButton);
    itemBar->addWidget(removeButton);
    itemBar->addWidget(upButton);
    itemBar->addWidget(downButton);
    itemBar->addStretch();
    itemBar->addWidget(new QLabel(
            "Paths are written exactly as shown; disk installation will validate their targets.", this));
    layout->addLayout(itemBar);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);

    connect(newButton, &QPushButton::clicked, this, &FhdbConfigDialog::createDefault);
    connect(openButton, &QPushButton::clicked, this, &FhdbConfigDialog::openConfig);
    connect(saveButton, &QPushButton::clicked, this, &FhdbConfigDialog::saveConfigAs);
    connect(addButton, &QPushButton::clicked, this, &FhdbConfigDialog::addItem);
    connect(removeButton, &QPushButton::clicked, this, &FhdbConfigDialog::removeItem);
    connect(upButton, &QPushButton::clicked, this, [this]() { moveItem(-1); });
    connect(downButton, &QPushButton::clicked, this, [this]() { moveItem(1); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    loadControls();
}

QString FhdbConfigDialog::currentPathText() const
{
    return currentPath.isEmpty() ? "New configuration (not saved)" : currentPath;
}

void FhdbConfigDialog::createDefault()
{
    if (QMessageBox::question(this, "Create default FHDB menu",
            "Replace the current editor contents with a new default FHDB menu?",
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    config = Ps2::FhdbConfig::CreateDefault();
    currentPath.clear();
    loadControls();
}

void FhdbConfigDialog::openConfig()
{
    const QString path = QFileDialog::getOpenFileName(this, "Open FREEHDB.CNF",
            currentPath, "FHDB configuration (FREEHDB.CNF *.CNF);;All files (*)");
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        QMessageBox::critical(this, "Open FHDB configuration", file.errorString());
        return;
    }
    const QByteArray data = file.readAll();
    config = Ps2::FhdbConfig::Parse(std::string(data.constData(),
            static_cast<std::size_t>(data.size())));
    currentPath = path;
    loadControls();
}

void FhdbConfigDialog::saveConfigAs()
{
    if (!storeControls())
        return;
    const QString suggested = currentPath.isEmpty() ? "FREEHDB.CNF" : currentPath;
    const QString path = QFileDialog::getSaveFileName(this, "Save FREEHDB.CNF",
            suggested, "FHDB configuration (FREEHDB.CNF *.CNF);;All files (*)");
    if (path.isEmpty())
        return;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        QMessageBox::critical(this, "Save FHDB configuration", file.errorString());
        return;
    }
    const std::string text = config.Serialize();
    if (file.write(text.data(), static_cast<qint64>(text.size())) !=
            static_cast<qint64>(text.size()) || !file.commit())
    {
        QMessageBox::critical(this, "Save FHDB configuration", file.errorString());
        return;
    }
    currentPath = path;
    pathLabel->setText(currentPathText());
}

void FhdbConfigDialog::loadControls()
{
    pathLabel->setText(currentPathText());
    fastBoot->setChecked(config.Value("FastBoot") != "0");
    const std::string action = config.Value("LK_Auto_E1");
    defaultAction->setText(QString::fromUtf8(action.empty() ? "OSDSYS" : action.c_str()));
    bool okay = false;
    int count = QString::fromStdString(config.Value("OSDSYS_num_displayed_items")).toInt(&okay);
    displayedItems->setValue(okay ? count : 7);

    menuTable->setRowCount(0);
    for (const Ps2::FhdbMenuItem &item : config.MenuItems())
        appendItem(item);
    if (menuTable->rowCount() != 0)
        menuTable->selectRow(0);
}

bool FhdbConfigDialog::storeControls()
{
    if (defaultAction->text().trimmed().isEmpty())
    {
        QMessageBox::warning(this, "Invalid FHDB menu", "The default boot action cannot be empty.");
        defaultAction->setFocus();
        return false;
    }

    std::vector<Ps2::FhdbMenuItem> items;
    std::set<int> usedNumbers;
    for (int row = 0; row < menuTable->rowCount(); row++)
    {
        bool okay = false;
        const int number = menuTable->item(row, 0)->text().toInt(&okay);
        const QString name = menuTable->item(row, 1)->text().trimmed();
        const QString path1 = menuTable->item(row, 2)->text().trimmed();
        if (!okay || number <= 0 || number > 999 || !usedNumbers.insert(number).second)
        {
            QMessageBox::warning(this, "Invalid FHDB menu",
                    QString("Row %1 must have a unique item number from 1 to 999.").arg(row + 1));
            menuTable->selectRow(row);
            return false;
        }
        if (name.isEmpty() || path1.isEmpty())
        {
            QMessageBox::warning(this, "Invalid FHDB menu",
                    QString("Row %1 requires both a menu name and primary path.").arg(row + 1));
            menuTable->selectRow(row);
            return false;
        }
        items.push_back({ number, name.toUtf8().toStdString(), path1.toUtf8().toStdString(),
                menuTable->item(row, 3)->text().trimmed().toUtf8().toStdString(),
                menuTable->item(row, 4)->text().trimmed().toUtf8().toStdString() });
    }
    std::sort(items.begin(), items.end(), [](const Ps2::FhdbMenuItem &left,
            const Ps2::FhdbMenuItem &right) { return left.index < right.index; });

    config.SetValue("FastBoot", fastBoot->isChecked() ? "1" : "0");
    config.SetValue("LK_Auto_E1", defaultAction->text().trimmed().toUtf8().toStdString());
    config.SetValue("LK_Auto_E3", defaultAction->text().trimmed().toUtf8().toStdString());
    config.SetValue("OSDSYS_num_displayed_items", std::to_string(displayedItems->value()));
    try
    {
        config.SetMenuItems(items);
    }
    catch (const std::exception &error)
    {
        QMessageBox::warning(this, "Invalid FHDB menu", QString::fromLocal8Bit(error.what()));
        return false;
    }
    return true;
}

void FhdbConfigDialog::appendItem(const Ps2::FhdbMenuItem &item)
{
    const int row = menuTable->rowCount();
    menuTable->insertRow(row);
    menuTable->setItem(row, 0, new QTableWidgetItem(QString::number(item.index)));
    menuTable->setItem(row, 1, new QTableWidgetItem(QString::fromUtf8(item.name.c_str())));
    menuTable->setItem(row, 2, new QTableWidgetItem(QString::fromUtf8(item.path1.c_str())));
    menuTable->setItem(row, 3, new QTableWidgetItem(QString::fromUtf8(item.path2.c_str())));
    menuTable->setItem(row, 4, new QTableWidgetItem(QString::fromUtf8(item.path3.c_str())));
}

int FhdbConfigDialog::nextItemNumber() const
{
    std::set<int> used;
    for (int row = 0; row < menuTable->rowCount(); row++)
        used.insert(menuTable->item(row, 0)->text().toInt());
    for (int number = 1; number <= 999; number++)
        if (used.find(number) == used.end())
            return number;
    return 0;
}

void FhdbConfigDialog::addItem()
{
    const int number = nextItemNumber();
    if (number == 0)
        return;
    appendItem({ number, "New Item", "hdd0:__sysconf:pfs:/FMCB/APP.ELF", "", "" });
    menuTable->selectRow(menuTable->rowCount() - 1);
    menuTable->editItem(menuTable->item(menuTable->rowCount() - 1, 1));
}

void FhdbConfigDialog::removeItem()
{
    const int row = menuTable->currentRow();
    if (row >= 0)
        menuTable->removeRow(row);
}

void FhdbConfigDialog::moveItem(int direction)
{
    const int row = menuTable->currentRow();
    const int target = row + direction;
    if (row < 0 || target < 0 || target >= menuTable->rowCount())
        return;

    // Item numbers determine FHDB's display order. Keep the number in each
    // position and exchange the editable menu contents.
    for (int column = 1; column < menuTable->columnCount(); column++)
    {
        const QString current = menuTable->item(row, column)->text();
        menuTable->item(row, column)->setText(menuTable->item(target, column)->text());
        menuTable->item(target, column)->setText(current);
    }
    menuTable->selectRow(target);
}
