#include "ui/TrendChart.h"

#include <QPainter>
#include <QPolygonF>

#include <algorithm>

TrendChart::TrendChart(const QString &caption, const QColor &color,
                       const QString &unit, QWidget *parent)
    : QWidget(parent)
    , m_caption(caption)
    , m_color(color)
    , m_unit(unit)
{
    setMinimumHeight(110);
}

void TrendChart::addSample(double v)
{
    if (v < 0) {
        v = 0;
    }
    m_values.append(v);
    if (m_values.size() > kMaxPoints) {
        m_values.remove(0, m_values.size() - kMaxPoints);
    }
    update();
}

void TrendChart::clear()
{
    m_values.clear();
    update();
}

void TrendChart::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int left = 46, right = 10, top = 20, bottom = 18;
    const QRectF plot(left, top, width() - left - right, height() - top - bottom);

    // 标题
    p.setPen(palette().color(QPalette::WindowText));
    p.drawText(QRectF(left, 2, plot.width(), 16), Qt::AlignLeft | Qt::AlignVCenter, m_caption);

    // 纵轴上限
    double maxV = m_fixedMax > 0 ? m_fixedMax : 1.0;
    if (m_fixedMax <= 0 && !m_values.isEmpty()) {
        maxV = std::max(1.0, *std::max_element(m_values.begin(), m_values.end()));
    }

    // 绘图区背景框
    p.setPen(QColor(0xCC, 0xCC, 0xCC));
    p.setBrush(QColor(0xFA, 0xFB, 0xFC));
    p.drawRect(plot);

    // y 轴刻度文字（上限 / 0）
    p.setPen(QColor(0x88, 0x88, 0x88));
    const QString top1 = m_unit.isEmpty() ? QString::number(maxV, 'g', 4)
                                          : QStringLiteral("%1%2").arg(maxV, 0, 'g', 4).arg(m_unit);
    p.drawText(QRectF(0, top - 6, left - 6, 14), Qt::AlignRight | Qt::AlignVCenter, top1);
    p.drawText(QRectF(0, plot.bottom() - 7, left - 6, 14), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("0"));

    // 折线
    if (m_values.size() >= 2) {
        const int n = m_values.size();
        QPolygonF poly;
        poly.reserve(n);
        for (int i = 0; i < n; ++i) {
            const double x = plot.left() + plot.width() * i / (n - 1);
            const double y = plot.bottom() - plot.height() * (m_values[i] / maxV);
            poly << QPointF(x, qBound(plot.top(), y, plot.bottom()));
        }
        p.setBrush(Qt::NoBrush);
        QPen pen(m_color);
        pen.setWidthF(1.6);
        p.setPen(pen);
        p.drawPolyline(poly);
    } else {
        p.setPen(QColor(0xAA, 0xAA, 0xAA));
        p.drawText(plot, Qt::AlignCenter, QStringLiteral("（测试开始后显示趋势）"));
    }
}
