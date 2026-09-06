#ifndef FHDBCONFIGDIALOG_H
#define FHDBCONFIGDIALOG_H

#include <QDialog>

#include "core/FhdbConfig.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;

class FhdbConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FhdbConfigDialog(QWidget *parent = nullptr);

private:
    void createDefault();
    void openConfig();
    void saveConfigAs();
    void loadControls();
    bool storeControls();
    void addItem();
    void removeItem();
    void moveItem(int direction);
    int nextItemNumber() const;
    void appendItem(const Ps2::FhdbMenuItem &item);
    QString currentPathText() const;

    Ps2::FhdbConfig config;
    QString currentPath;
    QLabel *pathLabel = nullptr;
    QCheckBox *fastBoot = nullptr;
    QLineEdit *defaultAction = nullptr;
    QSpinBox *displayedItems = nullptr;
    QTableWidget *menuTable = nullptr;
};

#endif // FHDBCONFIGDIALOG_H
