#include "Installwizard.h"
#include "splashwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QStyleFactory>
#include <QPalette>
#include <QFile>
#include <QIcon>
#include <QDir>
#include <QColor>
#include <QMessageBox>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <unistd.h>
#include <vector>

// ---------- Consistent look & feel helpers (works root or not) ----------
static void applyNeutralPalette(bool darkMode)
{
    QPalette p;
    if (darkMode) {
        // Legible dark palette tuned for Fusion
        p.setColor(QPalette::Window, QColor(37, 37, 38));
        p.setColor(QPalette::WindowText, QColor(220, 220, 220));
        p.setColor(QPalette::Base, QColor(30, 30, 30));
        p.setColor(QPalette::AlternateBase, QColor(37, 37, 38));
        p.setColor(QPalette::ToolTipBase, QColor(255, 255, 220));
        p.setColor(QPalette::ToolTipText, QColor(0, 0, 0));
        p.setColor(QPalette::Text, QColor(220, 220, 220));
        p.setColor(QPalette::Button, QColor(45, 45, 48));
        p.setColor(QPalette::ButtonText, QColor(220, 220, 220));
        p.setColor(QPalette::BrightText, Qt::red);
        p.setColor(QPalette::Highlight, QColor(14, 99, 156));
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::Link, QColor(90, 170, 255));
    } else {
        // Neutral light palette close to stock Fusion
        p = QPalette();
        p.setColor(QPalette::Highlight, QColor(33, 150, 243));
        p.setColor(QPalette::HighlightedText, Qt::white);
    }
    QApplication::setPalette(p);
}

static void forceConsistentLook(bool useDarkMode = false,
                                const QString &optionalQssPath = QString(),
                                const QString &optionalIconTheme = QString(),
                                const QStringList &optionalIconSearchPaths = {})
{
    // 1) Force cross-platform widget style so desktops can’t change shapes
    const QString fusion = QStringLiteral("Fusion");
    if (QStyleFactory::keys().contains(fusion, Qt::CaseInsensitive)) {
        QApplication::setStyle(QStyleFactory::create(fusion));
    }

    // 2) Apply a stable palette (dark or light)
    applyNeutralPalette(useDarkMode);

    // 3) (Optional) Load a QSS you ship (skip if empty)
    if (!optionalQssPath.isEmpty()) {
        QFile f(optionalQssPath);
        if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qApp->setStyleSheet(QString::fromUtf8(f.readAll()));
        }
    }

    // 4) (Optional) Icon theme control (useful when running as root)
    if (!optionalIconSearchPaths.isEmpty()) {
        QStringList paths = QIcon::themeSearchPaths();
        for (const QString &p : optionalIconSearchPaths)
            if (!paths.contains(p)) paths.prepend(p);
        QIcon::setThemeSearchPaths(paths);
    }
    if (!optionalIconTheme.isEmpty())
        QIcon::setThemeName(optionalIconTheme);
}
// -----------------------------------------------------------------------

int main(int argc, char *argv[])
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    // High-DPI settings should be set before QApplication (safe on Qt ≥5.6)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
#endif

    QApplication a(argc, argv);
    a.setWindowIcon(QIcon(":/img/app.ico"));

    // Lock the look & feel early so KDE/GNOME/Xfce and root/user look the same
    // Flip useDarkMode to false for light mode. You can add a QSS later.
    forceConsistentLook(
        /*useDarkMode=*/true,
        /*optionalQssPath=*/QString(),   // e.g. ":/styles/app.qss" if you add one
        /*optionalIconTheme=*/QString(), // e.g. "breeze" or leave empty
        /*optionalIconSearchPaths=*/{}   // e.g. {"/usr/share/icons"}
        );

    // Ensure the installer has the necessary privileges to run
    if (geteuid() != 0) {
        // Relaunch through pkexec (password dialog). Use execvp so polkit can talk to agent.
        QString path = QFileInfo(argv[0]).absoluteFilePath();

        QList<QByteArray> argBytes{"pkexec", "env"};
        QByteArray disp = qgetenv("DISPLAY");
        if (!disp.isEmpty())
            argBytes << QByteArray("DISPLAY=") + disp;
        QByteArray xauth = qgetenv("XAUTHORITY");
        if (!xauth.isEmpty())
            argBytes << QByteArray("XAUTHORITY=") + xauth;
        QByteArray qpa = qgetenv("QT_QPA_PLATFORMTHEME");
        if (!qpa.isEmpty())
            argBytes << QByteArray("QT_QPA_PLATFORMTHEME=") + qpa;
        argBytes << path.toLocal8Bit();

        std::vector<char*> execArgs;
        execArgs.reserve(argBytes.size() + 1);
        for (QByteArray &aBytes : argBytes)
            execArgs.push_back(aBytes.data());
        execArgs.push_back(nullptr);

        execvp("pkexec", execArgs.data());
        // If execvp returns, it failed; exit this instance.
        return 0;

        // (Unreachable on success; kept for completeness)
        QMessageBox::critical(nullptr, "Permissions Error",
                              "This installer must be run as root.\n"
                              "Please restart it using 'sudo' or 'pkexec'.");
        return 1;
    }

    // ---- Root instance from here on ----
    // Create wizard but do not show it yet
    auto *wizard = new Installwizard;

    // Center splash and wizard to the primary screen
    const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    const QSize wizSize = wizard->size().isValid() ? wizard->size()
                                                   : wizard->sizeHint().expandedTo(QSize(900, 600));
    const QPoint topLeft = screen.center() - QPoint(wizSize.width() / 2, wizSize.height() / 2);
    const QRect frame(topLeft, wizSize);
    wizard->setGeometry(frame);

    // Show splash then reveal the wizard after ~5 seconds
    auto *splash = new SplashWindow;
    splash->setGeometry(frame);
    splash->setCenterGif(":/img/arch_spin.gif", QSize(100, 100));
    splash->start(5000, wizard);   // SplashWindow::start(delayMs, QWidget* toShowAfter)

    return a.exec();
}
