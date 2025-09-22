#pragma once
#include <QWidget>
#include <QVector>

class QTimer;
class QLabel;
class QMovie;

class SplashWindow : public QWidget {
    Q_OBJECT
public:
    // Center a GIF (resource path like :/img/spinner.gif or a filesystem path)
    void setCenterGif(const QString &fileOrResource, const QSize &maxSize = QSize());
    explicit SplashWindow(QWidget *parent = nullptr);
    // Show the splash for `msecs` then optionally show `toShowAfter`
    void start(int msecs, QWidget *toShowAfter = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *e) override;   // to keep GIF centered on resize

private:
    void layoutLink();
    void layoutGIF();
    void initLink();
    QLabel *gifLabel_ = nullptr;
    QLabel *linkLabel_ = nullptr;
    QMovie *movie_    = nullptr;
    QSize   gifMaxSize_;
    void layoutGif();
    struct Drop { qreal x; qreal y; qreal speed; };
    QTimer *timer_;
    QVector<Drop> drops_;
    qreal angle_ = 0.0;

    void initDrops();
    void drawBackground(QPainter &p);
    void drawCodeRain(QPainter &p);
    void drawArchIcon(QPainter &p);
};
