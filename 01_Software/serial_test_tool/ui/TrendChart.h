#pragma once

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

// 轻量自绘折线图：把按时间采样的一串数值画成趋势曲线（不依赖 Qt Charts 模块）。
// 用于结果页展示“响应时间趋势”和“成功率趋势”。纯显示控件。
class TrendChart : public QWidget
{
    Q_OBJECT

public:
    explicit TrendChart(const QString &caption, const QColor &color,
                        const QString &unit = QString(), QWidget *parent = nullptr);

    void setFixedMax(double m) { m_fixedMax = m; update(); } // >0 固定纵轴上限（如成功率 100）
    void addSample(double v);                                // 追加一个采样点
    void clear();                                            // 清空（新测试开始时）

    QSize sizeHint() const override { return {360, 120}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_caption;
    QColor m_color;
    QString m_unit;
    double m_fixedMax = -1.0;          // <=0 表示自动按数据最大值缩放
    QVector<double> m_values;
    static constexpr int kMaxPoints = 1200; // 超出后丢弃最旧的点
};
