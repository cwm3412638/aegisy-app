#ifndef HELP_DIALOG_H
#define HELP_DIALOG_H

#include <QDialog>

class HelpDialog : public QDialog
{
    Q_OBJECT
public:
    explicit HelpDialog(QWidget *parent = nullptr);
};

#endif // HELP_DIALOG_H
