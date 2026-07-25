#include "long_long_spin_box.h"

#include <QLineEdit>

#include <algorithm>

LongLongSpinBox::LongLongSpinBox(QWidget *parent) : QAbstractSpinBox(parent)
{
    setValue(0);
    connect(this, &QAbstractSpinBox::editingFinished, this, &LongLongSpinBox::commitText);
}

qint64 LongLongSpinBox::value() const
{
    qint64 parsed = value_;
    return valueFromText(lineEdit()->text(), parsed) ? parsed : value_;
}

void LongLongSpinBox::setValue(qint64 value)
{
    value_ = std::clamp(value, minimum_, maximum_);
    lineEdit()->setText(textFromValue(value_));
}

void LongLongSpinBox::setRange(qint64 minimum, qint64 maximum)
{
    minimum_ = minimum;
    maximum_ = std::max(minimum, maximum);
    setValue(value_);
}

void LongLongSpinBox::setSingleStep(qint64 step)
{
    singleStep_ = std::max<qint64>(1, step);
}

void LongLongSpinBox::setSuffix(const QString &suffix)
{
    suffix_ = suffix;
    setValue(value());
}

void LongLongSpinBox::setSpecialValueText(const QString &text)
{
    specialValueText_ = text;
    setValue(value());
}

QValidator::State LongLongSpinBox::validate(QString &input, int &position) const
{
    Q_UNUSED(position);
    if (!specialValueText_.isEmpty() && input == specialValueText_)
        return QValidator::Acceptable;

    QString number = input;
    if (!suffix_.isEmpty() && number.endsWith(suffix_))
        number.chop(suffix_.size());
    number = number.trimmed();
    if (number.isEmpty() || number == QStringLiteral("-"))
        return QValidator::Intermediate;

    qint64 parsed = 0;
    if (!valueFromText(input, parsed))
        return QValidator::Invalid;
    return parsed >= minimum_ && parsed <= maximum_
        ? QValidator::Acceptable : QValidator::Invalid;
}

void LongLongSpinBox::fixup(QString &input) const
{
    qint64 parsed = value_;
    input = valueFromText(input, parsed)
        ? textFromValue(std::clamp(parsed, minimum_, maximum_))
        : textFromValue(value_);
}

void LongLongSpinBox::stepBy(int steps)
{
    commitText();
    qint64 next = value_;
    if (steps > 0) {
        const qint64 count = steps;
        next = singleStep_ > (maximum_ - value_) / count
            ? maximum_ : value_ + singleStep_ * count;
    } else if (steps < 0) {
        const qint64 count = -qint64(steps);
        next = singleStep_ > (value_ - minimum_) / count
            ? minimum_ : value_ - singleStep_ * count;
    }
    setValue(next);
}

QAbstractSpinBox::StepEnabled LongLongSpinBox::stepEnabled() const
{
    StepEnabled enabled = StepNone;
    const qint64 current = value();
    if (current > minimum_)
        enabled |= StepDownEnabled;
    if (current < maximum_)
        enabled |= StepUpEnabled;
    return enabled;
}

QString LongLongSpinBox::textFromValue(qint64 value) const
{
    if (value == minimum_ && !specialValueText_.isEmpty())
        return specialValueText_;
    return QString::number(value) + suffix_;
}

bool LongLongSpinBox::valueFromText(const QString &text, qint64 &value) const
{
    if (!specialValueText_.isEmpty() && text == specialValueText_) {
        value = minimum_;
        return true;
    }
    QString number = text;
    if (!suffix_.isEmpty() && number.endsWith(suffix_))
        number.chop(suffix_.size());
    bool ok = false;
    const qint64 parsed = number.trimmed().toLongLong(&ok);
    if (ok)
        value = parsed;
    return ok;
}

void LongLongSpinBox::commitText()
{
    setValue(value());
}
