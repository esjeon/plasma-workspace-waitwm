/*****************************************************************

Copyright 2000 Matthias Ettrich <ettrich@kde.org>
Copyright 2005 Lubos Lunak <l.lunak@kde.org>
Copyright 2018 David Edmundson <davidedmundson@kde.org>


relatively small extensions by Oswald Buddenhagen <ob6@inf.tu-dresden.de>

some code taken from the dcopserver (part of the KDE libraries), which is
Copyright 1999 Matthias Ettrich <ettrich@kde.org>
Copyright 1999 Preston Brown <pbrown@kde.org>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

******************************************************************/

#include "startup.h"

#include "debug.h"

#include "kcminit_interface.h"
#include "kded_interface.h"
#include <klauncher_interface.h>
#include "ksmserver_interface.h"

#include <KCompositeJob>
#include <Kdelibs4Migration>
#include <KIO/DesktopExecParser>
#include <KJob>
#include <KNotifyConfig>
#include <KProcess>
#include <KService>
#include <KConfigGroup>
#include <KWindowSystem>

#include <phonon/audiooutput.h>
#include <phonon/mediaobject.h>
#include <phonon/mediasource.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDir>
#include <QStandardPaths>
#include <QTimer>
#include <QProcess>

#include "startupadaptor.h"

class Phase: public KCompositeJob
{
Q_OBJECT
public:
    Phase(const AutoStart &autostart, QObject *parent)
        : KCompositeJob(parent)
        , m_autostart(autostart)
    {}

    bool addSubjob(KJob *job) override {
        bool rc = KCompositeJob::addSubjob(job);
        job->start();
        return rc;
    }

    void slotResult(KJob *job) override {
        KCompositeJob::slotResult(job);
        if (!hasSubjobs()) {
            emitResult();
        }
    }

protected:
    const AutoStart m_autostart;
};

class StartupPhase0: public Phase
{
Q_OBJECT
public:
    StartupPhase0(const AutoStart& autostart, QObject *parent) : Phase(autostart, parent)
    {}
    void start() override {
        qCDebug(PLASMA_SESSION) << "Phase 0";
        addSubjob(new AutoStartAppsJob(m_autostart, 0));
        addSubjob(new KCMInitJob(1));
        addSubjob(new SleepJob());
    }
};

class StartupPhase1: public Phase
{
Q_OBJECT
public:
    StartupPhase1(const AutoStart& autostart, QObject *parent) : Phase(autostart, parent)
    {}
    void start() override {
        qCDebug(PLASMA_SESSION) << "Phase 1";
        addSubjob(new AutoStartAppsJob(m_autostart, 1));
    }
};

class StartupPhase2: public Phase
{
Q_OBJECT
public:
    StartupPhase2(const AutoStart& autostart, QObject *parent) : Phase(autostart, parent)
    {}
    void runUserAutostart();
    bool migrateKDE4Autostart(const QString &folder);

    void start() override {
        qCDebug(PLASMA_SESSION) << "Phase 2";
        addSubjob(new AutoStartAppsJob(m_autostart, 2));
        addSubjob(new KDEDInitJob());
        addSubjob(new KCMInitJob(2));
        runUserAutostart();
    }
};

SleepJob::SleepJob()
{
}

void SleepJob::start()
{
    auto t = new QTimer(this);
    connect(t, &QTimer::timeout, this, [this]() {emitResult();});
    t->start(100);
}

// Put the notification in its own thread as it can happen that
// PulseAudio will start initializing with this, so let's not
// block the main thread with waiting for PulseAudio to start
class NotificationThread : public QThread
{
    Q_OBJECT
    void run() override {
        // We cannot parent to the thread itself so let's create
        // a QObject on the stack and parent everythign to it
        QObject parent;
        KNotifyConfig notifyConfig(QStringLiteral("plasma_workspace"), QList< QPair<QString,QString> >(), QStringLiteral("startkde"));
        const QString action = notifyConfig.readEntry(QStringLiteral("Action"));
        if (action.isEmpty() || !action.split(QLatin1Char('|')).contains(QLatin1String("Sound"))) {
            // no startup sound configured
            return;
        }
        Phonon::AudioOutput *m_audioOutput = new Phonon::AudioOutput(Phonon::NotificationCategory, &parent);

        QString soundFilename = notifyConfig.readEntry(QStringLiteral("Sound"));
        if (soundFilename.isEmpty()) {
            qCWarning(PLASMA_SESSION) << "Audio notification requested, but no sound file provided in notifyrc file, aborting audio notification";
            return;
        }

        QUrl soundURL;
        const auto dataLocations = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
        for (const QString &dataLocation: dataLocations) {
            soundURL = QUrl::fromUserInput(soundFilename,
                                           dataLocation + QStringLiteral("/sounds"),
                                           QUrl::AssumeLocalFile);
            if (soundURL.isLocalFile() && QFile::exists(soundURL.toLocalFile())) {
                break;
            } else if (!soundURL.isLocalFile() && soundURL.isValid()) {
                break;
            }
            soundURL.clear();
        }
        if (soundURL.isEmpty()) {
            qCWarning(PLASMA_SESSION) << "Audio notification requested, but sound file from notifyrc file was not found, aborting audio notification";
            return;
        }

        Phonon::MediaObject *m = new Phonon::MediaObject(&parent);
        connect(m, &Phonon::MediaObject::finished, this, &NotificationThread::quit);

        Phonon::createPath(m, m_audioOutput);

        m->setCurrentSource(soundURL);
        m->play();
        exec();
    }

};

Startup::Startup(QObject *parent):
    QObject(parent)
{
    new StartupAdaptor(this);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Startup"), QStringLiteral("org.kde.Startup"), this);
    QDBusConnection::sessionBus().registerService(QStringLiteral("org.kde.Startup"));

    const AutoStart autostart;

    auto phase0 = new StartupPhase0(autostart, this);
    auto phase1 = new StartupPhase1(autostart, this);
    auto phase2 = new StartupPhase2(autostart, this);
    auto restoreSession = new RestoreSessionJob();

    // this includes starting kwin (currently)
    // forward our arguments into ksmserver to match startplasma expectations
    QStringList arguments = qApp->arguments();
    arguments.removeFirst();
    auto ksmserverJob = new StartServiceJob(QStringLiteral("ksmserver"), arguments, QStringLiteral("org.kde.ksmserver"));
    auto wmjob = new WindowManagerWaitJob();

    connect(ksmserverJob, &KJob::finished, wmjob, &KJob::start);
    connect(wmjob, &KJob::finished, phase0, &KJob::start);

    connect(phase0, &KJob::finished, phase1, &KJob::start);

    connect(phase1, &KJob::finished, restoreSession, &KJob::start);
    connect(restoreSession, &KJob::finished, phase2, &KJob::start);
    upAndRunning(QStringLiteral("ksmserver"));

    connect(phase1, &KJob::finished, this, []() {
        NotificationThread *loginSound = new NotificationThread();
        connect(loginSound, &NotificationThread::finished, loginSound, &NotificationThread::deleteLater);
        loginSound->start();});
    connect(phase2, &KJob::finished, this, &Startup::finishStartup);

    ksmserverJob->start();
}

void Startup::upAndRunning( const QString& msg )
{
    QDBusMessage ksplashProgressMessage = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KSplash"),
                                                                         QStringLiteral("/KSplash"),
                                                                         QStringLiteral("org.kde.KSplash"),
                                                                         QStringLiteral("setStage"));
    ksplashProgressMessage.setArguments(QList<QVariant>() << msg);
    QDBusConnection::sessionBus().asyncCall(ksplashProgressMessage);
}

void Startup::finishStartup()
{
    qCDebug(PLASMA_SESSION) << "Finished";
    upAndRunning(QStringLiteral("ready"));
}

void Startup::updateLaunchEnv(const QString &key, const QString &value)
{
    qputenv(key.toLatin1(), value.toLatin1());
}

KCMInitJob::KCMInitJob(int phase)
    :m_phase(phase)
{
}

void KCMInitJob::start() {
    org::kde::KCMInit kcminit(QStringLiteral("org.kde.kcminit"),
                              QStringLiteral("/kcminit"),
                              QDBusConnection::sessionBus());
    kcminit.setTimeout(10 * 1000);

    QDBusPendingReply<void> pending;
    if (m_phase == 1) {
        pending = kcminit.runPhase1();
    } else {
        pending = kcminit.runPhase2();
    }
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this]() {emitResult();});
    connect(watcher, &QDBusPendingCallWatcher::finished, watcher, &QObject::deleteLater);
}

KDEDInitJob::KDEDInitJob()
{
}

void KDEDInitJob::start() {
    qCDebug(PLASMA_SESSION());
    org::kde::kded5 kded( QStringLiteral("org.kde.kded5"),
                         QStringLiteral("/kded"),
                         QDBusConnection::sessionBus());
    auto pending = kded.loadSecondPhase();

    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this]() {emitResult();});
    connect(watcher, &QDBusPendingCallWatcher::finished, watcher, &QObject::deleteLater);
}

RestoreSessionJob::RestoreSessionJob():
    KJob()
{}

void RestoreSessionJob::start()
{
    OrgKdeKSMServerInterfaceInterface ksmserverIface(QStringLiteral("org.kde.ksmserver"), QStringLiteral("/KSMServer"), QDBusConnection::sessionBus());
    auto pending = ksmserverIface.restoreSession();

    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this]() {emitResult();});
    connect(watcher, &QDBusPendingCallWatcher::finished, watcher, &QObject::deleteLater);
}

void StartupPhase2::runUserAutostart()
{
    // Now let's execute the scripts in the KDE-specific autostart-scripts folder.
    const QString autostartFolder = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QDir::separator() + QStringLiteral("autostart-scripts");

    QDir dir(autostartFolder);
    if (!dir.exists()) {
        // Create dir in all cases, so that users can find it :-)
        dir.mkpath(QStringLiteral("."));

        if (!migrateKDE4Autostart(autostartFolder)) {
            return;
        }
    }

    const QStringList entries = dir.entryList(QDir::Files);
    foreach (const QString &file, entries) {
        // Don't execute backup files
        if (!file.endsWith(QLatin1Char('~')) && !file.endsWith(QLatin1String(".bak")) &&
                (file[0] != QLatin1Char('%') || !file.endsWith(QLatin1Char('%'))) &&
                (file[0] != QLatin1Char('#') || !file.endsWith(QLatin1Char('#'))))
        {
            const QString fullPath = dir.absolutePath() + QLatin1Char('/') + file;

            qCInfo(PLASMA_SESSION) << "Starting autostart script " << fullPath;
            auto p = new KProcess; //deleted in onFinished lambda
            p->setProgram(fullPath);
            p->start();
            connect(p, static_cast<void (QProcess::*)(int)>(&QProcess::finished), [p](int exitCode) {
                qCInfo(PLASMA_SESSION) << "autostart script" << p->program() << "finished with exit code " << exitCode;
                p->deleteLater();
            });
        }
    }
}

bool StartupPhase2::migrateKDE4Autostart(const QString &autostartFolder)
{
    // Migrate user autostart from kde4
    Kdelibs4Migration migration;
    if (!migration.kdeHomeFound()) {
        return false;
    }
    // KDEHOME/Autostart was the default value for KGlobalSettings::autostart()
    QString oldAutostart = migration.kdeHome() + QStringLiteral("/Autostart");
    // That path could be customized in kdeglobals
    const QString oldKdeGlobals = migration.locateLocal("config", QStringLiteral("kdeglobals"));
    if (!oldKdeGlobals.isEmpty()) {
        oldAutostart = KConfig(oldKdeGlobals).group("Paths").readEntry("Autostart", oldAutostart);
    }

    const QDir oldFolder(oldAutostart);
    qCDebug(PLASMA_SESSION) << "Copying autostart files from" << oldFolder.path();
    const QStringList entries = oldFolder.entryList(QDir::Files);
    foreach (const QString &file, entries) {
        const QString src = oldFolder.absolutePath() + QLatin1Char('/') + file;
        const QString dest = autostartFolder + QLatin1Char('/') + file;
        QFileInfo info(src);
        bool success;
        if (info.isSymLink()) {
            // This will only work with absolute symlink targets
            success = QFile::link(info.symLinkTarget(), dest);
        } else {
            success = QFile::copy(src, dest);
        }
        if (!success) {
            qCWarning(PLASMA_SESSION) << "Error copying" << src << "to" << dest;
        }
    }
    return true;
}

AutoStartAppsJob::AutoStartAppsJob(const AutoStart & autostart, int phase)
    : m_autoStart(autostart)
{
    m_autoStart.setPhase(phase);
}

void AutoStartAppsJob::start() {
    qCDebug(PLASMA_SESSION);

    QTimer::singleShot(0, this, [=]() {
        do {
            QString serviceName = m_autoStart.startService();
            if (serviceName.isEmpty()) {
                // Done
                if (!m_autoStart.phaseDone()) {
                    m_autoStart.setPhaseDone();
                }
                emitResult();
                return;
            }
            KService service(serviceName);
            auto arguments = KIO::DesktopExecParser(service, QList<QUrl>()).resultingArguments();
            if (arguments.isEmpty()) {
                qCWarning(PLASMA_SESSION) << "failed to parse" << serviceName << "for autostart";
                continue;
            }
            qCInfo(PLASMA_SESSION) << "Starting autostart service " << serviceName << arguments;
            auto program = arguments.takeFirst();
            if (!QProcess::startDetached(program, arguments))
                qCWarning(PLASMA_SESSION) << "could not start" << serviceName << ":" << program << arguments;
        } while (true);
    });
}


StartServiceJob::StartServiceJob(const QString &process, const QStringList &args, const QString &serviceId):
    KJob(),
    m_process(process),
    m_args(args)
{
    auto watcher = new QDBusServiceWatcher(serviceId, QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForRegistration, this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, &StartServiceJob::emitResult);
}

void StartServiceJob::start()
{
    QProcess::startDetached(m_process, m_args);
}


WindowManagerWaitJob::WindowManagerWaitJob()
{
    m_enabled = true;
}

void WindowManagerWaitJob::start()
{
    // do not wait if compositor is already up
    if (KWindowSystem::compositingActive()) {
        qCInfo(PLASMA_SESSION) << "WindowManagerWaitJob: skipping";
        slotOnReady(true);
        return;
    }

    // delay to make sure WM fully initializes
    auto timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this]() {
        qCInfo(PLASMA_SESSION) << "WindowManagerWaitJob: timeout reached";
        slotOnReady(true);
    });

    timer->start(3000);

    // cut the delay if compositor is detected
    connect(KWindowSystem::self(), &KWindowSystem::compositingChanged, this,
        [this](bool active) {
            qCInfo(PLASMA_SESSION) << "WindowManagerWaitJob: KWindoSystem triggered";
            slotOnReady(active);
        }
    );

    auto kwinWatcher = new QDBusServiceWatcher(
            QStringLiteral("org.kde.KWin"),
            QDBusConnection::sessionBus(),
            QDBusServiceWatcher::WatchForRegistration,
            this);
    connect(kwinWatcher, &QDBusServiceWatcher::serviceRegistered, this, [this]() {
        if (!m_enabled)
            return;

        qCInfo(PLASMA_SESSION) << "WindowManagerWaitJob: found KWin";
        // KWin is ready, wait for its compositor
        QDBusConnection::sessionBus().connect(
                QStringLiteral("org.kde.KWin"),
                QStringLiteral("/Compositor"),
                QStringLiteral("org.kde.kwin.Compositing"),
                QStringLiteral("compositingToggled"),
                this, SLOT(slotOnReady(bool)));
    });
}

void WindowManagerWaitJob::slotOnReady(bool active)
{
    if (m_enabled && active) {
        m_enabled = false;
        emitResult();
    }
}

#include "startup.moc"
