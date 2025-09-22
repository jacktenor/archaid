#include "splashwindow.h"
#include <QLabel>
#include <QMovie>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QPen>
#include <QFont>
#include <QTimer>
#include <QScreen>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QMargins>

SplashWindow::SplashWindow(QWidget *parent) : QWidget(parent), timer_(new QTimer(this)) {
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);

    // Size & center on primary screen
    const QSize sz(500, 420);
    resize(sz);
    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    move(screen.center() - QPoint(width() / 2, height() / 2));

    initDrops();

    initLink();                // <-- ADD: create the bottom hyperlink label
    layoutLink();              // <-- ADD: place it correctly at the bottom

    // ~60 FPS animation
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, [this]() {
        angle_ += 2.2; if (angle_ >= 360.0) angle_ -= 360.0;

        // advance drops
        for (auto &d : drops_) {
            d.y += d.speed;
            if (d.y > height() + 40) {
                d.x = QRandomGenerator::global()->bounded(0, width());
                d.y = -QRandomGenerator::global()->bounded(0, height());
                d.speed = 2.5 + QRandomGenerator::global()->generateDouble() * (6.5 - 2.5);
            }
        }
        update();
    });
}

void SplashWindow::initLink() {
    if (linkLabel_) return;

    linkLabel_ = new QLabel(this);
    linkLabel_->setObjectName("bottomLink");
    linkLabel_->setText(
        "<a href=\"https://beeralator.com\">Beeralator.com</a>"
    );
    linkLabel_->setAlignment(Qt::AlignCenter);
    linkLabel_->setTextFormat(Qt::RichText);
    linkLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    linkLabel_->setOpenExternalLinks(true);   // lets Qt open the URL in the default browser
    linkLabel_->setCursor(Qt::PointingHandCursor);

    // Style: subtle, readable, with hover feedback
    linkLabel_->setStyleSheet(
        "QLabel#bottomLink {"
        "   color: #8ae4ff;"
        "   background: transparent;"
        "   font-weight: 500;"
        "   font-size: 16px;"
        "   padding: 4px 8px;"
        "}"
        "QLabel#bottomLink:hover {"
        "   color: #FFFFFF;"
        "   text-decoration: underline;"
        "}"
    );

    // Optional: if you want to intercept and handle clicks yourself (not required):
    // QObject::connect(linkLabel_, &QLabel::linkActivated, this, [](const QString &url){
    //     QDesktopServices::openUrl(QUrl(url));
    // });
}

void SplashWindow::layoutLink() {
    if (!linkLabel_) return;

    // Keep inside the rounded “card” margins you used (8 px) plus a little breathing room
    const int hMargin = 16;      // left/right insets
    const int vPadding = 16;     // distance from bottom edge
    const int minHeight = linkLabel_->sizeHint().height();

    // Make a full-width region (minus margins), center text inside
    const int w = qMax(100, width() - hMargin * 2);
    const int x = hMargin;
    const int y = height() - vPadding - minHeight;

    linkLabel_->setGeometry(QRect(x, y, w, minHeight));
    linkLabel_->raise();
    linkLabel_->show();
}


void SplashWindow::setCenterGif(const QString &fileOrResource, const QSize &maxSize)
{
    if (!gifLabel_) {
        gifLabel_ = new QLabel(this);
        gifLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
        gifLabel_->setStyleSheet("background: transparent;");
        gifLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    if (movie_) {
        movie_->stop();
        movie_->deleteLater();
        movie_ = nullptr;
    }

    movie_ = new QMovie(fileOrResource, QByteArray(), this);
    if (maxSize.isValid())
        movie_->setScaledSize(maxSize);
    gifMaxSize_ = maxSize;

    gifLabel_->setMovie(movie_);
    gifLabel_->adjustSize();

    // If the GIF reports a different frame size later, keep it centered
    connect(movie_, &QMovie::frameChanged, this, [this](int){ layoutGif(); });

    layoutGif();
    movie_->start();
}

void SplashWindow::layoutGif()
{
    if (!gifLabel_) return;

    // Preferred size: scaled size if provided; otherwise current frame; otherwise hint
    QSize sz = gifLabel_->sizeHint();
    if (movie_) {
        QSize frameSz = movie_->currentPixmap().size();
        if (gifMaxSize_.isValid())
            frameSz = gifMaxSize_;
        if (!frameSz.isEmpty())
            sz = frameSz;
    }

    const QRect r = rect();
    const QPoint p((r.width() - sz.width())/2, (r.height() - sz.height())/2);
    gifLabel_->setGeometry(QRect(p, sz));
    gifLabel_->show();
}

void SplashWindow::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    layoutGif();
    layoutLink();   // <-- ADD: keep the hyperlink pinned to the bottom on resize
}

void SplashWindow::start(int msecs, QWidget *toShowAfter) {
    show();
    timer_->start();
    QTimer::singleShot(msecs, this, [this, toShowAfter]() {
        timer_->stop();
        if (toShowAfter) toShowAfter->show();
        close(); // window is lightweight; auto-destroy with parent/OS
    });
}

// REPLACE initDrops() with this
void SplashWindow::initDrops() {
    drops_.clear();
    const int cols = 40; // number of code columns
    for (int i = 0; i < cols; ++i) {
        Drop d;
        d.x = QRandomGenerator::global()->bounded(0, width());
        d.y = QRandomGenerator::global()->bounded(-height(), height());
        // FIX: no bounded(double,double) — use generateDouble scaling
        d.speed = 2.5 + QRandomGenerator::global()->generateDouble() * (6.5 - 2.5);
        drops_.push_back(d);
    }
}


void SplashWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    drawBackground(p);
    drawCodeRain(p);
    // drawArchIcon(p);
}

void SplashWindow::drawBackground(QPainter &p)
{
    // Fill the whole window (we'll still draw rounded “card” edges)
    const QRect full = rect();
    p.fillRect(full, QColor(7, 0, 140)); // deep navy base

    // Rounded dark-blue card with a subtle diagonal gradient
    const QRect r = full.marginsRemoved(QMargins(8, 8, 8, 8));
    QPainterPath path; path.addRoundedRect(r, 20, 20);

    QLinearGradient g(r.topLeft(), r.bottomRight());
    g.setColorAt(0.0, QColor(29, 1, 3));  // #0b1f5b
    g.setColorAt(1.0, QColor(0, 0, 144));   // #06142c
    p.fillPath(path, g);

    // Soft outer glow (blue)
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 8; ++i) {
        QColor c(8, 8, 25, 5 - i); // subtle blue aura
        QPainterPath glow; glow.addRoundedRect(r.adjusted(-i*2, -i*2, i*2, i*2), 24 + i, 24 + i);
        p.fillPath(glow, c);
    }
}

void SplashWindow::drawCodeRain(QPainter &p)
{
    QFont f("Monospace"); f.setStyleHint(QFont::TypeWriter); f.setPointSize(11);
    p.setFont(f);

    for (const auto &d : drops_) {
        int y = static_cast<int>(d.y);
        const int step = 18;
        for (int k = 0; k < 10; ++k) {
            const int yy = y - k * step;
            if (yy < 0 || yy > height()) continue;
            const int ch = QRandomGenerator::global()->bounded(0, 2) ? '0' : '1';
            const int alpha = 225 - k * 20; // fade tail
            p.setPen(QColor(49, 245, 75, qBound(0, alpha, 255))); // white glyphs
            p.drawText(QPointF(d.x, yy), QString(QChar(ch)));
        }
    }


    // Subtitle text
    p.setPen(QColor(106, 196, 235));  // 10, 170, 11
    QFont ff = p.font(); ff.setPointSize(28); ff.setBold(true);
    p.setFont(ff);
    p.drawText(QRect(0, height() - 375, width(), 32), Qt::AlignCenter, "ArchAid");

    p.setPen(QColor(227, 227, 227, 227));
    QFont fff = p.font(); fff.setPointSize(12); fff.setBold(true);
    p.setFont(fff);
    p.drawText(QRect(0, height() - 325, width(), 32), Qt::AlignCenter, " A basic Arch Linux install helper");
}
