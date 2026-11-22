// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Aaron Rainbolt <arraybolt3@gmail.com>
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <KAboutData>
#include <KirigamiAddons/App/KirigamiAppDefaults>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLocale>
#include <QLockFile>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSettings>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>
#include <QTranslator>
#include <QWindow>
#include <QtQml>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "app/karton.h"
#include "app/karton_private.h"
#include "app/shortcutinhibitor.h"
#include "karton_debug.h"
#include "karton_version.h"

static bool qmlCheckFailed = false;
static bool verboseLogging = false;

namespace
{
int unixSignalFds[2] = {-1, -1};

// write() is async-signal-safe, so the handler only wakes the shutdown thread
// through a socket pair.
void unixSignalHandler(int)
{
    const char byte = 1;
    [[maybe_unused]] const ssize_t written = ::write(unixSignalFds[1], &byte, 1);
}

// A SIGTERM used to kill the process without running aboutToQuit, which left
// KDE global shortcuts blocked for the whole session because a viewer had
// called KGlobalAccel.blockGlobalShortcuts(true). Test runs and session
// shutdown both deliver SIGTERM, so it must shut down gracefully. The cleanup
// cannot rely on the GUI thread: a blocking libvirt console read keeps it from
// ever reaching the event loop, so a separate thread performs the fallback.
void installTerminationHandler(QCoreApplication *app)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, unixSignalFds) != 0) {
        qCWarning(KARTON_DEBUG) << "Failed to create the termination signal socket pair:" << errno;
        return;
    }
    std::thread([app]() {
        char byte = 0;
        while (::read(unixSignalFds[0], &byte, 1) < 0 && errno == EINTR) { }
        qCInfo(KARTON_DEBUG) << "Termination signal received, shutting down";
        // Let the event loop quit gracefully first: aboutToQuit releases the
        // shortcut block, the keyboard grab and the instance lock.
        QMetaObject::invokeMethod(
            app,
            []() {
                QCoreApplication::quit();
            },
            Qt::QueuedConnection);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        // The GUI thread did not finish (blocked read or stuck teardown).
        // Release the KDE-wide shortcut block from this thread and exit.
        qCWarning(KARTON_DEBUG) << "Forcing shutdown after a termination signal";
        KartonShortcuts::unblockGlobalShortcuts(500);
        std::_Exit(0);
    }).detach();

    struct sigaction action = {};
    action.sa_handler = unixSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (const int signalNumber : {SIGTERM, SIGINT, SIGHUP}) {
        sigaction(signalNumber, &action, nullptr);
    }
}
} // namespace

static void libvirtErrorHandler(void *, virErrorPtr error)
{
    if (!verboseLogging || !error || !error->message) {
        return;
    }
    qWarning("libvirt: %s", error->message);
}

static void qmlCheckMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Q_UNUSED(context);
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        if (message.contains(QStringLiteral("qrc:/qt/qml/org/kde/karton/"))
            && (message.contains(QStringLiteral("Cannot")) || message.contains(QStringLiteral("non-existent"))
                || message.contains(QStringLiteral("unavailable")) || message.contains(QStringLiteral("failed to load"))
                || message.contains(QStringLiteral("as it is null")))) {
            qmlCheckFailed = true;
        }
    }
    fprintf(stderr, "%s\n", qPrintable(message));
}

static QSGRendererInterface::GraphicsApi preferredGraphicsApi()
{
    const QByteArray requested = qgetenv("KARTON_GRAPHICS_API").trimmed().toLower();
    if (requested == "software") {
        return QSGRendererInterface::Software;
    }
    if (requested == "vulkan" || requested == "vk") {
        // Qt's Vulkan/Wayland swapchain stops presenting after the first
        // fullscreen transition (Qt 6.12 + NVIDIA), so Vulkan is not supported.
        qCWarning(KARTON_DEBUG) << "Vulkan rendering is not supported anymore; using OpenGL";
    }
    return QSGRendererInterface::OpenGL;
}

static void raiseMainWindow(int attempts = 10)
{
    const QWindowList windows = QGuiApplication::topLevelWindows();
    for (QWindow *window : windows) {
        if (window->objectName() != QStringLiteral("mainWindow")) {
            continue;
        }
        if (window->visibility() == QWindow::Minimized) {
            window->showNormal();
        } else {
            window->show();
        }
        window->raise();
        window->requestActivate();
        return;
    }
    if (attempts > 0) {
        // The second instance can reach the socket while the first one is
        // still constructing its main window.
        QTimer::singleShot(100, [attempts]() {
            raiseMainWindow(attempts - 1);
        });
    }
}

static bool notifyRunningInstance(const QString &socketName)
{
    QLocalSocket socket;
    for (int attempt = 0; attempt < 6; ++attempt) {
        socket.connectToServer(socketName);
        if (socket.waitForConnected(80)) {
            socket.write("raise");
            socket.flush();
            socket.waitForBytesWritten(200);
            socket.disconnectFromServer();
            return true;
        }
        QThread::msleep(20);
    }
    return false;
}

int main(int argc, char *argv[])
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    // Present on the compositor's frame callbacks / swap interval and avoid
    // tearing. The guest image does not need multisampling.
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setSwapInterval(1);
    format.setSamples(0);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    verboseLogging = app.arguments().contains(QStringLiteral("--verbose")) || !qEnvironmentVariableIsEmpty("KARTON_VERBOSE");
    QLoggingCategory::setFilterRules(verboseLogging ? QStringLiteral("*.debug=true\n*.info=true") : QStringLiteral("*.debug=false\n*.info=false"));
    virSetErrorFunc(nullptr, libvirtErrorHandler);
    app.setApplicationVersion(QStringLiteral(KARTON_VERSION_STRING));
    KirigamiAppDefaults::apply(&app);

    const QSGRendererInterface::GraphicsApi graphicsApi = preferredGraphicsApi();
    QQuickWindow::setGraphicsApi(graphicsApi);
    qCInfo(KARTON_DEBUG) << "Using graphics API:" << (graphicsApi == QSGRendererInterface::OpenGL ? "OpenGL" : "software");

    // Translations are embedded in the binary by qt_add_translations and
    // looked up from :/i18n first, so the resource always matches this build.
    QTranslator *translator = new QTranslator(&app);
    auto loadLanguage = [&](const QString &requested) {
        QCoreApplication::removeTranslator(translator);
        const QString language = Karton::resolveLanguage(requested, QLocale::system().name());
        const QString resourcePath = QStringLiteral(":/i18n/") + language + QStringLiteral(".qm");
        const QString installedPath = QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("karton/translations/%1.qm").arg(language));
        const QString localPath = QCoreApplication::applicationDirPath() + QLatin1Char('/') + language + QStringLiteral(".qm");
        if (translator->load(resourcePath) || (!installedPath.isEmpty() && translator->load(installedPath)) || translator->load(localPath)) {
            QCoreApplication::installTranslator(translator);
        }
    };

    const QSettings languageSettings(KartonCommon::kartonConfigPath(), QSettings::IniFormat);
    loadLanguage(languageSettings.value(QStringLiteral("ui/language"), QStringLiteral("auto")).toString());

    KAboutData about(QStringLiteral("karton"),
                     QCoreApplication::translate("Main", "karton"),
                     QStringLiteral(KARTON_VERSION_STRING),
                     QCoreApplication::translate("Main", "manage_and_run_virtual_machines_with_ease"),
                     KAboutLicense::GPL_V3,
                     QCoreApplication::translate("Main", "c_kde"));
    about.setDesktopFileName(QStringLiteral("org.kde.karton"));

    about.addAuthor(QCoreApplication::translate("Main", "aaron_rainbolt"),
                    QCoreApplication::translate("Main", "initial_developer"),
                    QStringLiteral("arraybolt3@gmail.com"));
    about.addAuthor(QCoreApplication::translate("Main", "derek_lin"),
                    QCoreApplication::translate("Main", "developer"),
                    QStringLiteral("derekhongdalin@gmail.com"));
    about.setTranslator(QCoreApplication::translate("Main", "your_names"), QCoreApplication::translate("Main", "your_emails"));

    KAboutData::setApplicationData(about);
    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("org.kde.karton")));

    const bool checkQml = QCoreApplication::arguments().contains(QStringLiteral("--check-qml"));

    // Only one main window may exist. Viewers are separate windows inside the
    // primary process, so a second process is never needed for them.
    std::unique_ptr<QLockFile> instanceLock;
    std::unique_ptr<QLocalServer> instanceServer;
    if (!checkQml) {
        QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (runtimeDir.isEmpty()) {
            runtimeDir = QDir::tempPath();
        }
        const QString lockPath = runtimeDir + QStringLiteral("/karton.lock");
        const QString socketName = runtimeDir + QStringLiteral("/karton.socket");

        instanceLock = std::make_unique<QLockFile>(lockPath);
        instanceLock->setStaleLockTime(0);
        if (!instanceLock->tryLock(100)) {
            qCInfo(KARTON_DEBUG) << "Another Karton instance is already running.";
            if (!notifyRunningInstance(socketName)) {
                fprintf(stderr, "Karton is already running but did not answer the single-instance socket.\n");
            }
            return 0;
        }

        instanceServer = std::make_unique<QLocalServer>();
        QLocalServer::removeServer(socketName);
        if (instanceServer->listen(socketName)) {
            QObject::connect(instanceServer.get(), &QLocalServer::newConnection, instanceServer.get(), [server = instanceServer.get()]() {
                while (QLocalSocket *socket = server->nextPendingConnection()) {
                    socket->readAll();
                    raiseMainWindow();
                    socket->disconnectFromServer();
                    socket->deleteLater();
                }
            });
        } else {
            qCWarning(KARTON_DEBUG) << "Failed to start the single-instance server:" << instanceServer->errorString();
        }
    }

    QQmlApplicationEngine engine;

    qCInfo(KARTON_DEBUG) << "Hello! Starting Karton...";

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&instanceLock]() {
        qCInfo(KARTON_DEBUG) << "Shutting down: releasing the global shortcut block";
        if (auto *karton = Karton::self()) {
            karton->releaseGlobalShortcuts();
        }
        qCInfo(KARTON_DEBUG) << "Shutting down: global shortcut block released";
        if (instanceLock) {
            instanceLock->unlock();
        }
        // Qt Wayland/NVIDIA can abort while destroying platform windows during
        // application teardown. All Karton state has already been persisted by
        // the QML close handlers; exit once the event loop has stopped.
        std::_Exit(0);
    });

    if (checkQml) {
        qInstallMessageHandler(qmlCheckMessageHandler);
    }
    engine.loadFromModule("org.kde.karton", "Main");

    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    if (!checkQml) {
        // Connect only after the main window has painted its first frame. A
        // slow or unresponsive libvirt must never keep the window hidden or
        // leave it without a "connecting" state.
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst())) {
            QObject::connect(window, &QQuickWindow::frameSwapped, Karton::self(), &Karton::start, Qt::SingleShotConnection);
        }
        // Software or headless runs may never report a frame swap.
        QTimer::singleShot(2000, Karton::self(), &Karton::start);
    }

    QObject::connect(Karton::self(), &Karton::languageChanged, &app, [&engine, &loadLanguage]() {
        loadLanguage(Karton::self()->language());
        engine.retranslate();
        // Domain state strings are generated on the C++ side; refresh them so
        // the list does not keep showing the previous language until the next
        // libvirt event.
        Karton::self()->refreshDomainList();
    });

    // Used by the opt-in test suite to catch QML component/import errors
    // without entering the event loop or requiring a graphical session.
    if (checkQml) {
        // Main.qml alone does not instantiate VMViewerWindow and the dialogs
        // that are created only on user action. Load the smoke component too,
        // so invalid property assignments in those components fail this test.
        engine.loadFromModule("org.kde.karton", "QmlSmokeTest");
        if (engine.rootObjects().isEmpty()) {
            return -1;
        }
        // Let the event loop deliver delayed component creation and bindings;
        // otherwise a null visual item or a late binding error can escape the
        // smoke test. QML starts helper threads, so exit without Qt teardown.
        QTimer::singleShot(500, &app, []() {
            std::_Exit(qmlCheckFailed ? 1 : 0);
        });
        return app.exec();
    }

    // Installed after all startup work so a signal that arrives before the
    // event loop runs still takes the default action; by then no shortcut
    // block can have been set yet.
    installTerminationHandler(&app);
    return app.exec();
}
