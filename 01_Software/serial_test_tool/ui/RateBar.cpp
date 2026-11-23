#include "ui/RateBar.h"

#include <QFontMetrics>
#include <QPainter>

RateBar::RateBar(const QString &caption, const QColor &fillColor, QWidget *parent)
    : QWidget(parent), m_caption(caption), m_fill(fillColor)
{
    setMinimumHeight(62);
}

void RateBar::setRate(double pct)
{
    m_na = (pct < 0.0);
    m_pct = m_na ? 0.0 : qBound(0.0, pct, 100.0);
    update();
}

QSize RateBar::sizeHint() const
{
    return {280, 62};
}

void RateBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QString text = m_na ? QStringLiteral("N/A") : QStringLiteral("%1%").arg(m_pct, 0, 'f', 2);
    p.setPen(palette().color(QPalette::WindowText));
    p.drawText(QRect(0, 0, width(), 22), Qt::AlignLeft | Qt::AlignVCenter, m_caption);
    QFont valueFont = font();
    valueFont.setPointSize(17);
    valueFont.setBold(true);
    p.setFont(valueFont);
    p.drawText(QRect(0, 22, width(), 30), Qt::AlignLeft | Qt::AlignVCenter, text);
    const QRectF track(0, 56, width(), 6);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#e5ebf4"));
    p.drawRoundedRect(track, 3, 3);
    if (!m_na && m_pct > 0.0) {
        // Clip to the track so very small percentages cannot paint outside it.
        p.save();
        p.setClipRect(QRectF(0, 56, width() * m_pct / 100.0, 6));
        p.setBrush(m_fill);
        p.drawRoundedRect(track, 3, 3);
        p.restore();
    }
}
