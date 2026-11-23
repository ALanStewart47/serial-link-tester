#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

// 指标卡片：标题、大号百分比与下方细进度条；文字始终保持高对比度。
// 纯显示控件，用于结果页图形化展示丢包率/正确率/总成功率。
class RateBar : public QWidget
{
    Q_OBJECT

public:
    explicit RateBar(const QString &caption, const QColor &fillColor, QWidget *parent = nullptr);

    // pct 取 0~100；传负值表示 N/A（不显示填充，文字显示 "N/A"）。
    void setRate(double pct);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_caption;
    QColor m_fill;
    double m_pct = 0.0;
    bool m_na = false;
};
