#pragma once

#include <QAbstractSpinBox>
#include <QValidator>

#include <limits>

class LongLongSpinBox final : public QAbstractSpinBox {
public:
    explicit LongLongSpinBox(QWidget *parent = nullptr);

    qint64 value() const;
    void setValue(qint64 value);
    void setRange(qint64 minimum, qint64 maximum);
    void setSingleStep(qint64 step);
    void setSuffix(const QString &suffix);
    void setSpecialValueText(const QString &text);

protected:
    QValidator::State validate(QString &input, int &position) const override;
    void fixup(QString &input) const override;
    void stepBy(int steps) override;
    StepEnabled stepEnabled() const override;

private:
    QString textFromValue(qint64 value) const;
    bool valueFromText(const QString &text, qint64 &value) const;
    void commitText();

    qint64 minimum_ = 0;
    qint64 maximum_ = std::numeric_limits<qint64>::max();
    qint64 value_ = 0;
    qint64 singleStep_ = 1;
    QString suffix_;
    QString specialValueText_;
};
