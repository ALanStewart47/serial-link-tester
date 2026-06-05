#include "ui/RateBar.h"

#include <QFontMetrics>
#include <QPainter>

namespace {
constexpr int kCaptionWidth = 96;
constexpr int kBarHeight = 22;
constexpr int kRadius = 4;
}

RateBar::RateBar(const QString &caption, const QColor &fillColor, QWidget *parent)
    : QWidget(parent)
    , m_caption(caption)
    , m_fill(fillColor)
{
    setMinimumHeight(kBarHeight + 6);
}

void RateBar::setRate(double pct)
{
    m_na = (pct < 0.0);
    m_pct = m_na ? 0.0 : qBound(0.0, pct, 100.0);
    update();
}

QSize RateBar::sizeHint() const
{
    return {320, kBarHeight + 6};
}

void RateBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int y = (height() - kBarHeight) / 2;

    // 标题
    p.setPen(palette().color(QPalette::WindowText));
    p.drawText(QRect(0, y, kCaptionWidth - 8, kBarHeight),
               Qt::AlignVCenter | Qt::AlignLeft, m_caption);

    // 轨道
    const QRect track(kCaptionWidth, y, width() - kCaptionWidth, kBarHeight);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xE6, 0xE6, 0xE6));
    p.drawRoundedRect(track, kRadius, kRadius);

    // 填充
    if (!m_na && m_pct > 0.0) {
        QRect fill = track;
        fill.setWidth(static_cast<int>(track.width() * m_pct / 100.0));
        p.setBrush(m_fill);
        p.drawRoundedRect(fill, kRadius, kRadius);
    }

    // 文字（百分比 / N/A）
    const QString text = m_na ? QStringLiteral("N/A")
                              : QStringLiteral("%1%").arg(m_pct, 0, 'f', 2);
    p.setPen(Qt::black);
    p.drawText(track, Qt::AlignCenter, text);
}
