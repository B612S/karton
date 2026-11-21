// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"
#include "app/libvirtconnection.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSGRendererInterface>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <grp.h>
#include <memory>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "domain/domain.h"
#include "domain/domainconfig.h"
#include "domain/domainxmlbuilder.h"
#include "domain/domainxmlreader.h"
#include "karton_debug.h"
#include "libvirt/libvirteventloop.h"
#include "libvirt/libvirtmonitor.h"
#include "os/osiconmapper.h"
#include "util/calltimer.h"

using namespace KartonCommon;

namespace
{
// The guest-agent flags are only valid for the current guest image. Store a
// small fingerprint so changing the system disk, its format or the OS metadata
// makes Karton re-evaluate the hint instead of trusting a stale "agent seen".
QString guestAgentPrefix(const Domain *domain)
{
    return QStringLiteral("guestAgent/%1/").arg(domain->config()->uuid());
}

QString guestAgentDiskPath(const Domain *domain)
{
    if (!domain) {
        return {};
    }
    // While a VM is running, its inactive configuration can already point at a
    // replacement image that will only be used after the next start. Attribute
    // the current agent connection to the live image instead.
    if (domain->isActive() && domain->domainPtr()) {
        char *xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
        if (xml) {
            QString livePath;
            QDomDocument document;
            if (document.setContent(QString::fromUtf8(xml))) {
                const QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
                const QDomNodeList disks = devices.elementsByTagName(QStringLiteral("disk"));
                for (int i = 0; i < disks.count(); ++i) {
                    const QDomElement disk = disks.at(i).toElement();
                    if (disk.attribute(QStringLiteral("device")) != QStringLiteral("disk")) {
                        continue;
                    }
                    const QString source = disk.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file"));
                    if (!source.isEmpty()) {
                        livePath = source;
                        break;
                    }
                }
            }
            free(xml);
            if (!livePath.isEmpty()) {
                return livePath;
            }
        }
    }
    return domain->config()->virtualDiskPath();
}

QString guestAgentFingerprint(const Domain *domain)
{
    const DomainConfig *config = domain->config();
    const QStringList parts{
        config->uuid(),
        config->osId(),
        guestAgentDiskPath(domain),
        config->diskFormat(),
        config->firmware(),
    };
    return parts.join(QLatin1Char('\n'));
}

void prepareGuestAgentState(QSettings &settings, const QString &prefix, const QString &fingerprint)
{
    if (settings.value(prefix + QStringLiteral("fingerprint")).toString() == fingerprint) {
        return;
    }
    settings.remove(prefix + QStringLiteral("seen"));
    settings.remove(prefix + QStringLiteral("warningShown"));
    settings.setValue(prefix + QStringLiteral("fingerprint"), fingerprint);
}

int aclBits(const QString &permissions)
{
    int bits = 0;
    if (permissions.contains(QLatin1Char('r'))) {
        bits |= R_OK;
    }
    if (permissions.contains(QLatin1Char('w'))) {
        bits |= W_OK;
    }
    if (permissions.contains(QLatin1Char('x'))) {
        bits |= X_OK;
    }
    return bits;
}

int modeBitsFor(const struct stat &info, uid_t uid, const QVector<gid_t> &groups)
{
    if (info.st_uid == uid) {
        return (info.st_mode >> 6) & 7;
    }
    if (groups.contains(info.st_gid)) {
        return (info.st_mode >> 3) & 7;
    }
    return info.st_mode & 7;
}

bool qemuCanAccess(const QString &path, uid_t uid, const QVector<gid_t> &groups, int required)
{
    struct stat info = {};
    const QByteArray nativePath = path.toLocal8Bit();
    if (::stat(nativePath.constData(), &info) != 0) {
        return false;
    }

    // ACL entries can only add permissions on top of the traditional mode
    // bits. When those bits already grant access, skip the getfacl process:
    // this check runs for every path component on every folder selection.
    if ((modeBitsFor(info, uid, groups) & required) == required) {
        return true;
    }

    QProcess process;
    process.start(QStringLiteral("getfacl"), {QStringLiteral("-n"), QStringLiteral("-c"), QStringLiteral("-p"), path});
    if (!process.waitForFinished(1500) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        // No getfacl available: fall back to the traditional mode bits.
        return (modeBitsFor(info, uid, groups) & required) == required;
    }

    int ownerBits = 0;
    int namedUserBits = -1;
    int groupBits = -1;
    int namedGroupBits = 0;
    bool groupMatched = false;
    int mask = 7;
    int otherBits = 0;

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QStringList fields = line.split(QLatin1Char(':'));
        if (fields.size() < 3) {
            continue;
        }
        const QString &tag = fields.at(0);
        const QString &qualifier = fields.at(1);
        const int bits = aclBits(fields.at(2));
        if (tag == QStringLiteral("user")) {
            if (qualifier.isEmpty()) {
                ownerBits = bits;
            } else if (qualifier.toUInt() == uid) {
                namedUserBits = bits;
            }
        } else if (tag == QStringLiteral("group")) {
            if (qualifier.isEmpty()) {
                groupBits = bits;
            } else if (groups.contains(qualifier.toUInt())) {
                groupMatched = true;
                namedGroupBits |= bits;
            }
        } else if (tag == QStringLiteral("mask")) {
            mask = bits;
        } else if (tag == QStringLiteral("other")) {
            otherBits = bits;
        }
    }

    const bool owningGroup = groups.contains(info.st_gid) && groupBits >= 0;
    groupMatched = groupMatched || owningGroup;
    const int groupClass = (owningGroup ? groupBits : 0) | namedGroupBits;
    int effective = otherBits;
    if (uid == info.st_uid) {
        effective = ownerBits;
    } else if (namedUserBits >= 0) {
        effective = namedUserBits & mask;
    } else if (groupMatched) {
        effective = groupClass & mask;
    }
    return (effective & required) == required;
}

// setfacl is spawned from the GUI thread while granting folder access. A hung
// network mount must not leave the dialog frozen forever, so cap the wait.
bool runSetfacl(const QStringList &arguments)
{
    QProcess process;
    process.start(QStringLiteral("setfacl"), arguments);
    if (!process.waitForStarted(5000)) {
        return false;
    }
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        qCWarning(KARTON_DEBUG) << "setfacl timed out for" << arguments;
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
} // namespace

QVariantMap Karton::mainWindowSize() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    QVariantMap size;
    const int width = settings.value(QStringLiteral("window/mainWidth"), 0).toInt();
    const int height = settings.value(QStringLiteral("window/mainHeight"), 0).toInt();
    if (width > 0 && height > 0) {
        size.insert(QStringLiteral("width"), width);
        size.insert(QStringLiteral("height"), height);
    }
    return size;
}

void Karton::saveMainWindowSize(int width, int height)
{
    if (width < 320 || height < 240) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("window/mainWidth"), width);
    settings.setValue(QStringLiteral("window/mainHeight"), height);
}

QVariantMap Karton::viewerWindowSize(const Domain *domain) const
{
    QVariantMap size;
    if (!domain) {
        return size;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("window/viewer/%1/").arg(domain->config()->uuid());
    const int width = settings.value(prefix + QStringLiteral("width"), 0).toInt();
    const int height = settings.value(prefix + QStringLiteral("height"), 0).toInt();
    if (width > 0 && height > 0) {
        size.insert(QStringLiteral("width"), width);
        size.insert(QStringLiteral("height"), height);
    }
    return size;
}

void Karton::saveViewerWindowSize(const Domain *domain, int width, int height)
{
    if (!domain || width < 320 || height < 240) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("window/viewer/%1/").arg(domain->config()->uuid());
    settings.setValue(prefix + QStringLiteral("width"), width);
    settings.setValue(prefix + QStringLiteral("height"), height);
}

int Karton::clipboardTextMode() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return qBound(0, settings.value(QStringLiteral("clipboard/textMode"), 3).toInt(), 3);
}

void Karton::saveClipboardTextMode(int mode)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("clipboard/textMode"), qBound(0, mode, 3));
    Q_EMIT clipboardSettingsChanged();
}

int Karton::clipboardFilesMode() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return qBound(0, settings.value(QStringLiteral("clipboard/filesMode"), 0).toInt(), 3);
}

void Karton::saveClipboardFilesMode(int mode)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("clipboard/filesMode"), qBound(0, mode, 3));
    Q_EMIT clipboardSettingsChanged();
}

int Karton::globalConsoleFontSize() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return qBound(8, settings.value(QStringLiteral("console/fontSize"), 12).toInt(), 96);
}

void Karton::saveGlobalConsoleFontSize(int pixels)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("console/fontSize"), qBound(8, pixels, 96));
    Q_EMIT consoleFontSizeChanged();
}

QString Karton::consoleFontFamily() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString family = settings.value(QStringLiteral("console/fontFamily"), QStringLiteral("monospace")).toString().trimmed();
    return family.isEmpty() ? QStringLiteral("monospace") : family;
}

void Karton::saveConsoleFontFamily(const QString &family)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString normalized = family.trimmed().isEmpty() ? QStringLiteral("monospace") : family.trimmed();
    settings.setValue(QStringLiteral("console/fontFamily"), normalized);
    Q_EMIT consoleFontFamilyChanged();
}

QVariantList Karton::consoleThemes() const
{
    const auto makeTheme = [](const QString &id, const QString &name, const QString &background, const QString &foreground, const QStringList &ansi) {
        QVariantMap theme;
        theme.insert(QStringLiteral("id"), id);
        theme.insert(QStringLiteral("name"), name);
        theme.insert(QStringLiteral("background"), background);
        theme.insert(QStringLiteral("foreground"), foreground);
        theme.insert(QStringLiteral("ansi"), ansi);
        return theme;
    };

    const QStringList tango = {
        QStringLiteral("#000000"),
        QStringLiteral("#cc0000"),
        QStringLiteral("#4e9a06"),
        QStringLiteral("#c4a000"),
        QStringLiteral("#3465a4"),
        QStringLiteral("#75507b"),
        QStringLiteral("#06989a"),
        QStringLiteral("#d3d7cf"),
        QStringLiteral("#555753"),
        QStringLiteral("#ef2929"),
        QStringLiteral("#8ae234"),
        QStringLiteral("#fce94f"),
        QStringLiteral("#729fcf"),
        QStringLiteral("#ad7fa8"),
        QStringLiteral("#34e2e2"),
        QStringLiteral("#eeeeec"),
    };
    const QStringList solarized = {
        QStringLiteral("#073642"),
        QStringLiteral("#dc322f"),
        QStringLiteral("#859900"),
        QStringLiteral("#b58900"),
        QStringLiteral("#268bd2"),
        QStringLiteral("#d33682"),
        QStringLiteral("#2aa198"),
        QStringLiteral("#eee8d5"),
        QStringLiteral("#002b36"),
        QStringLiteral("#cb4b16"),
        QStringLiteral("#586e75"),
        QStringLiteral("#657b83"),
        QStringLiteral("#839496"),
        QStringLiteral("#6c71c4"),
        QStringLiteral("#93a1a1"),
        QStringLiteral("#fdf6e3"),
    };
    const QStringList monokai = {
        QStringLiteral("#272822"),
        QStringLiteral("#f92672"),
        QStringLiteral("#a6e22e"),
        QStringLiteral("#f4bf75"),
        QStringLiteral("#66d9ef"),
        QStringLiteral("#ae81ff"),
        QStringLiteral("#a1efe4"),
        QStringLiteral("#f8f8f2"),
        QStringLiteral("#75715e"),
        QStringLiteral("#f92672"),
        QStringLiteral("#a6e22e"),
        QStringLiteral("#f4bf75"),
        QStringLiteral("#66d9ef"),
        QStringLiteral("#ae81ff"),
        QStringLiteral("#a1efe4"),
        QStringLiteral("#f9f8f5"),
    };
    const QStringList dracula = {
        QStringLiteral("#21222c"),
        QStringLiteral("#ff5555"),
        QStringLiteral("#50fa7b"),
        QStringLiteral("#f1fa8c"),
        QStringLiteral("#bd93f9"),
        QStringLiteral("#ff79c6"),
        QStringLiteral("#8be9fd"),
        QStringLiteral("#f8f8f2"),
        QStringLiteral("#6272a4"),
        QStringLiteral("#ff6e6e"),
        QStringLiteral("#69ff94"),
        QStringLiteral("#ffffa5"),
        QStringLiteral("#d6acff"),
        QStringLiteral("#ff92df"),
        QStringLiteral("#a4ffff"),
        QStringLiteral("#ffffff"),
    };
    const QStringList nord = {
        QStringLiteral("#3b4252"),
        QStringLiteral("#bf616a"),
        QStringLiteral("#a3be8c"),
        QStringLiteral("#ebcb8b"),
        QStringLiteral("#81a1c1"),
        QStringLiteral("#b48ead"),
        QStringLiteral("#88c0d0"),
        QStringLiteral("#e5e9f0"),
        QStringLiteral("#4c566a"),
        QStringLiteral("#bf616a"),
        QStringLiteral("#a3be8c"),
        QStringLiteral("#ebcb8b"),
        QStringLiteral("#81a1c1"),
        QStringLiteral("#b48ead"),
        QStringLiteral("#8fbcbb"),
        QStringLiteral("#eceff4"),
    };
    const QStringList gruvbox = {
        QStringLiteral("#282828"),
        QStringLiteral("#cc241d"),
        QStringLiteral("#98971a"),
        QStringLiteral("#d79921"),
        QStringLiteral("#458588"),
        QStringLiteral("#b16286"),
        QStringLiteral("#689d6a"),
        QStringLiteral("#a89984"),
        QStringLiteral("#928374"),
        QStringLiteral("#fb4934"),
        QStringLiteral("#b8bb26"),
        QStringLiteral("#fabd2f"),
        QStringLiteral("#83a598"),
        QStringLiteral("#d3869b"),
        QStringLiteral("#8ec07c"),
        QStringLiteral("#ebdbb2"),
    };
    const QStringList green = {
        QStringLiteral("#000000"),
        QStringLiteral("#00aa00"),
        QStringLiteral("#00ff00"),
        QStringLiteral("#00cc00"),
        QStringLiteral("#00ff88"),
        QStringLiteral("#00aa00"),
        QStringLiteral("#00ffcc"),
        QStringLiteral("#ccffcc"),
        QStringLiteral("#005500"),
        QStringLiteral("#00ff00"),
        QStringLiteral("#88ff88"),
        QStringLiteral("#00ff00"),
        QStringLiteral("#00ffff"),
        QStringLiteral("#00ff00"),
        QStringLiteral("#00ffff"),
        QStringLiteral("#ffffff"),
    };

    return {
        makeTheme(QStringLiteral("karton-dark"), QStringLiteral("Karton Dark"), QStringLiteral("#101010"), QStringLiteral("#d0d0d0"), tango),
        makeTheme(QStringLiteral("breeze-dark"), QStringLiteral("Breeze Dark"), QStringLiteral("#232629"), QStringLiteral("#eff0f1"), tango),
        makeTheme(QStringLiteral("breeze-light"), QStringLiteral("Breeze Light"), QStringLiteral("#eff0f1"), QStringLiteral("#232629"), tango),
        makeTheme(QStringLiteral("solarized-dark"), QStringLiteral("Solarized Dark"), QStringLiteral("#002b36"), QStringLiteral("#839496"), solarized),
        makeTheme(QStringLiteral("solarized-light"), QStringLiteral("Solarized Light"), QStringLiteral("#fdf6e3"), QStringLiteral("#657b83"), solarized),
        makeTheme(QStringLiteral("monokai"), QStringLiteral("Monokai"), QStringLiteral("#272822"), QStringLiteral("#f8f8f2"), monokai),
        makeTheme(QStringLiteral("dracula"), QStringLiteral("Dracula"), QStringLiteral("#282a36"), QStringLiteral("#f8f8f2"), dracula),
        makeTheme(QStringLiteral("nord"), QStringLiteral("Nord"), QStringLiteral("#2e3440"), QStringLiteral("#d8dee9"), nord),
        makeTheme(QStringLiteral("gruvbox-dark"), QStringLiteral("Gruvbox Dark"), QStringLiteral("#282828"), QStringLiteral("#ebdbb2"), gruvbox),
        makeTheme(QStringLiteral("green-on-black"), QStringLiteral("Green on Black"), QStringLiteral("#000000"), QStringLiteral("#00ff00"), green),
    };
}

QString Karton::consoleTheme() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString id = settings.value(QStringLiteral("console/theme"), QStringLiteral("karton-dark")).toString();
    const QVariantList themes = consoleThemes();
    for (const QVariant &entry : themes) {
        if (entry.toMap().value(QStringLiteral("id")).toString() == id) {
            return id;
        }
    }
    return QStringLiteral("karton-dark");
}

void Karton::saveConsoleTheme(const QString &themeId)
{
    QString normalized = themeId;
    bool found = false;
    const QVariantList themes = consoleThemes();
    for (const QVariant &entry : themes) {
        if (entry.toMap().value(QStringLiteral("id")).toString() == normalized) {
            found = true;
            break;
        }
    }
    if (!found) {
        normalized = QStringLiteral("karton-dark");
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("console/theme"), normalized);
    Q_EMIT consoleThemeChanged();
}

int Karton::domainConsoleFontSize(const Domain *domain) const
{
    if (!domain) {
        return 0;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const int value = settings.value(QStringLiteral("console/%1/fontSize").arg(domain->config()->uuid()), 0).toInt();
    return value > 0 ? qBound(8, value, 96) : 0;
}

void Karton::saveDomainConsoleFontSize(const Domain *domain, int pixels)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString key = QStringLiteral("console/%1/fontSize").arg(domain->config()->uuid());
    if (pixels <= 0) {
        settings.remove(key);
    } else {
        settings.setValue(key, qBound(8, pixels, 96));
    }
    Q_EMIT consoleFontSizeChanged();
}

int Karton::consoleFontSize(const Domain *domain) const
{
    const int overrideSize = domainConsoleFontSize(domain);
    return overrideSize > 0 ? overrideSize : globalConsoleFontSize();
}

QString Karton::consoleCursorShape() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString shape = settings.value(QStringLiteral("console/cursorShape"), QStringLiteral("block")).toString();
    return shape == QStringLiteral("bar") || shape == QStringLiteral("underline") ? shape : QStringLiteral("block");
}

void Karton::saveConsoleCursorShape(const QString &shape)
{
    const QString normalized = shape == QStringLiteral("bar") || shape == QStringLiteral("underline") ? shape : QStringLiteral("block");
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("console/cursorShape"), normalized);
    Q_EMIT consoleCursorShapeChanged();
}

int Karton::xmlEditorFontSize() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return qBound(8, settings.value(QStringLiteral("editor/fontSize"), 12).toInt(), 48);
}

void Karton::saveXmlEditorFontSize(int pixels)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("editor/fontSize"), qBound(8, pixels, 48));
    Q_EMIT xmlEditorSettingsChanged();
}

QString Karton::xmlEditorFontFamily() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString family = settings.value(QStringLiteral("editor/fontFamily"), QStringLiteral("monospace")).toString().trimmed();
    return family.isEmpty() ? QStringLiteral("monospace") : family;
}

void Karton::saveXmlEditorFontFamily(const QString &family)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString normalized = family.trimmed().isEmpty() ? QStringLiteral("monospace") : family.trimmed();
    settings.setValue(QStringLiteral("editor/fontFamily"), normalized);
    Q_EMIT xmlEditorSettingsChanged();
}

QVariantList Karton::xmlEditorThemes() const
{
    const auto makeTheme = [](const QString &id,
                              const QString &name,
                              const QString &background,
                              const QString &text,
                              const QString &tag,
                              const QString &attribute,
                              const QString &value,
                              const QString &comment,
                              const QString &declaration,
                              bool system = false) {
        QVariantMap theme;
        theme.insert(QStringLiteral("id"), id);
        theme.insert(QStringLiteral("name"), name);
        theme.insert(QStringLiteral("system"), system);
        theme.insert(QStringLiteral("background"), background);
        theme.insert(QStringLiteral("text"), text);
        theme.insert(QStringLiteral("tag"), tag);
        theme.insert(QStringLiteral("attribute"), attribute);
        theme.insert(QStringLiteral("value"), value);
        theme.insert(QStringLiteral("comment"), comment);
        theme.insert(QStringLiteral("declaration"), declaration);
        return theme;
    };

    return {
        makeTheme(QStringLiteral("system"), QCoreApplication::translate("Karton", "system_theme"), {}, {}, {}, {}, {}, {}, {}, true),
        makeTheme(QStringLiteral("breeze-dark"),
                  QStringLiteral("Breeze Dark"),
                  QStringLiteral("#232629"),
                  QStringLiteral("#eff0f1"),
                  QStringLiteral("#3daee9"),
                  QStringLiteral("#f67400"),
                  QStringLiteral("#27ae60"),
                  QStringLiteral("#7f8c8d"),
                  QStringLiteral("#9b59b6")),
        makeTheme(QStringLiteral("breeze-light"),
                  QStringLiteral("Breeze Light"),
                  QStringLiteral("#eff0f1"),
                  QStringLiteral("#232629"),
                  QStringLiteral("#2980b9"),
                  QStringLiteral("#b35c00"),
                  QStringLiteral("#1e8449"),
                  QStringLiteral("#7f8c8d"),
                  QStringLiteral("#8e44ad")),
        makeTheme(QStringLiteral("solarized-dark"),
                  QStringLiteral("Solarized Dark"),
                  QStringLiteral("#002b36"),
                  QStringLiteral("#839496"),
                  QStringLiteral("#268bd2"),
                  QStringLiteral("#b58900"),
                  QStringLiteral("#2aa198"),
                  QStringLiteral("#586e75"),
                  QStringLiteral("#6c71c4")),
        makeTheme(QStringLiteral("solarized-light"),
                  QStringLiteral("Solarized Light"),
                  QStringLiteral("#fdf6e3"),
                  QStringLiteral("#657b83"),
                  QStringLiteral("#268bd2"),
                  QStringLiteral("#b58900"),
                  QStringLiteral("#2aa198"),
                  QStringLiteral("#93a1a1"),
                  QStringLiteral("#6c71c4")),
        makeTheme(QStringLiteral("monokai"),
                  QStringLiteral("Monokai"),
                  QStringLiteral("#272822"),
                  QStringLiteral("#f8f8f2"),
                  QStringLiteral("#f92672"),
                  QStringLiteral("#fd971f"),
                  QStringLiteral("#a6e22e"),
                  QStringLiteral("#75715e"),
                  QStringLiteral("#ae81ff")),
        makeTheme(QStringLiteral("dracula"),
                  QStringLiteral("Dracula"),
                  QStringLiteral("#282a36"),
                  QStringLiteral("#f8f8f2"),
                  QStringLiteral("#ff79c6"),
                  QStringLiteral("#ffb86c"),
                  QStringLiteral("#50fa7b"),
                  QStringLiteral("#6272a4"),
                  QStringLiteral("#bd93f9")),
        makeTheme(QStringLiteral("nord"),
                  QStringLiteral("Nord"),
                  QStringLiteral("#2e3440"),
                  QStringLiteral("#d8dee9"),
                  QStringLiteral("#88c0d0"),
                  QStringLiteral("#ebcb8b"),
                  QStringLiteral("#a3be8c"),
                  QStringLiteral("#616e88"),
                  QStringLiteral("#b48ead")),
        makeTheme(QStringLiteral("gruvbox-dark"),
                  QStringLiteral("Gruvbox Dark"),
                  QStringLiteral("#282828"),
                  QStringLiteral("#ebdbb2"),
                  QStringLiteral("#83a598"),
                  QStringLiteral("#fabd2f"),
                  QStringLiteral("#b8bb26"),
                  QStringLiteral("#928374"),
                  QStringLiteral("#d3869b")),
        makeTheme(QStringLiteral("green-on-black"),
                  QStringLiteral("Green on Black"),
                  QStringLiteral("#000000"),
                  QStringLiteral("#00ff00"),
                  QStringLiteral("#00ff00"),
                  QStringLiteral("#00cc00"),
                  QStringLiteral("#00ff88"),
                  QStringLiteral("#008800"),
                  QStringLiteral("#00aa00")),
    };
}

QString Karton::xmlEditorTheme() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString id = settings.value(QStringLiteral("editor/theme"), QStringLiteral("system")).toString();
    const QVariantList themes = xmlEditorThemes();
    for (const QVariant &entry : themes) {
        if (entry.toMap().value(QStringLiteral("id")).toString() == id) {
            return id;
        }
    }
    return QStringLiteral("system");
}

void Karton::saveXmlEditorTheme(const QString &themeId)
{
    QString normalized = themeId;
    bool found = false;
    const QVariantList themes = xmlEditorThemes();
    for (const QVariant &entry : themes) {
        if (entry.toMap().value(QStringLiteral("id")).toString() == normalized) {
            found = true;
            break;
        }
    }
    if (!found) {
        normalized = QStringLiteral("system");
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("editor/theme"), normalized);
    Q_EMIT xmlEditorSettingsChanged();
}

QStringList Karton::monospaceFontFamilies() const
{
    QStringList families;
    const QStringList all = QFontDatabase::families();
    for (const QString &family : all) {
        if (QFontDatabase::isFixedPitch(family)) {
            families.append(family);
        }
    }
    families.removeDuplicates();
    families.sort(Qt::CaseInsensitive);
    families.removeAll(QStringLiteral("monospace"));
    families.prepend(QStringLiteral("monospace"));
    return families;
}

int Karton::cleanupStaleCaches()
{
    if (!m_conn) {
        return 0;
    }

    QSet<QString> knownUuids;
    for (const Domain *domain : std::as_const(m_domains)) {
        knownUuids.insert(domain->config()->uuid());
    }
    for (const LibvirtConnection *connection : std::as_const(m_extraConnections)) {
        for (const Domain *domain : connection->domains()) {
            knownUuids.insert(domain->config()->uuid());
        }
    }

    int removed = 0;
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QStringList prefixes = {QStringLiteral("window/viewer"), QStringLiteral("audio"), QStringLiteral("display"), QStringLiteral("console")};
    for (const QString &prefix : prefixes) {
        settings.beginGroup(prefix);
        const QStringList groups = settings.childGroups();
        settings.endGroup();
        for (const QString &group : groups) {
            if (!knownUuids.contains(group)) {
                settings.remove(prefix + QLatin1Char('/') + group);
                ++removed;
            }
        }
    }
    settings.sync();
    return removed;
}

void Karton::removeDomainCache(const QString &uuid)
{
    if (uuid.isEmpty()) {
        return;
    }

    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QStringList prefixes = {QStringLiteral("window/viewer"), QStringLiteral("audio"), QStringLiteral("display"), QStringLiteral("console")};
    for (const QString &prefix : prefixes) {
        settings.remove(prefix + QLatin1Char('/') + uuid);
    }
    settings.sync();
}

QString Karton::defaultImagesDirectory() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/libvirt/images");
}

QString Karton::imagesDirectory() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString configured = settings.value(QStringLiteral("storage/imagesDirectory")).toString().trimmed();
    return configured.isEmpty() ? defaultImagesDirectory() : QDir::cleanPath(configured);
}

void Karton::saveImagesDirectory(const QString &path)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty() || QDir::cleanPath(trimmed) == defaultImagesDirectory()) {
        settings.remove(QStringLiteral("storage/imagesDirectory"));
    } else {
        settings.setValue(QStringLiteral("storage/imagesDirectory"), QDir::cleanPath(trimmed));
    }
    Q_EMIT imagesDirectoryChanged();
}

QString Karton::ensureDiskAccess(const QString &path, const QString &connectionUri, bool readOnly)
{
    ScopedCallTimer timer("Karton::ensureDiskAccess");
    if (path.trimmed().isEmpty()) {
        return {};
    }
    const QString uri = connectionUri.isEmpty() ? (m_activeUri.isEmpty() ? this->connectionUri() : m_activeUri) : connectionUri;
    if (connectionIsRemote(uri)) {
        return {};
    }
    // Only qemu:///system needs the libvirt QEMU user ACL. A session QEMU
    // process runs as the Karton user; remote paths are managed on the host.
    const QUrl url(uri);
    if (url.path() != QStringLiteral("/system")) {
        return {};
    }
    const QString targetPath = QDir::cleanPath(path);
    // Storage pool volumes are owned and chowned by libvirt itself; Karton
    // must not (and usually cannot) add ACLs inside a pool directory.
    if (isStorageVolumePath(targetPath, uri)) {
        return {};
    }

    struct passwd *qemuUser = getpwnam("libvirt-qemu");
    if (!qemuUser) {
        qemuUser = getpwnam("qemu");
    }
    if (!qemuUser) {
        return QCoreApplication::translate("Karton", "could_not_find_the_libvirt_qemu_user");
    }

    gid_t groupBuffer[256];
    int groupCount = 256;
    if (getgrouplist(qemuUser->pw_name, qemuUser->pw_gid, groupBuffer, &groupCount) < 0) {
        groupCount = 1;
    }
    QVector<gid_t> groups;
    groups.reserve(groupCount);
    for (int i = 0; i < groupCount; ++i) {
        groups.append(groupBuffer[i]);
    }

    const QFileInfo target(targetPath);
    if (!target.exists()) {
        // The file is created by the caller; ensure it again after creation.
        return {};
    }

    // Traverse-only access on every parent directory (including the home
    // directory and the disk folder). Never grant write access to a
    // directory: write access is limited to the disk file itself.
    QStringList directories;
    if (target.isDir()) {
        directories.append(targetPath);
    }
    QString parent = target.absolutePath();
    while (!parent.isEmpty() && parent != QStringLiteral("/")) {
        directories.append(parent);
        parent = QFileInfo(parent).absolutePath();
    }
    directories.removeDuplicates();
    for (const QString &directory : std::as_const(directories)) {
        if (qemuCanAccess(directory, qemuUser->pw_uid, groups, X_OK)) {
            continue;
        }
        const QFileInfo info(directory);
        if (info.ownerId() != static_cast<uint>(::getuid())) {
            return QCoreApplication::translate("Karton", "cannot_grant_access_to_1_because_it_is_owned_by_another_user").arg(directory);
        }
        if (!runSetfacl({QStringLiteral("-m"), QStringLiteral("u:%1:x").arg(QString::fromUtf8(qemuUser->pw_name)), directory})) {
            return QCoreApplication::translate("Karton", "failed_to_grant_libvirt_access_to_1").arg(directory);
        }
        if (!qemuCanAccess(directory, qemuUser->pw_uid, groups, X_OK)) {
            return QCoreApplication::translate("Karton", "libvirt_still_cannot_access_1_after_granting_permissions").arg(directory);
        }
    }

    if (target.isDir()) {
        return {};
    }

    // Read/write for disks; read-only for ISO images and other media. The
    // ACL is on the file, so moving the image and repointing the disk applies
    // the same rule to the new location automatically.
    const int required = readOnly ? R_OK : (R_OK | W_OK);
    if (qemuCanAccess(targetPath, qemuUser->pw_uid, groups, required)) {
        return {};
    }
    if (target.ownerId() != static_cast<uint>(::getuid())) {
        return QCoreApplication::translate("Karton", "cannot_grant_access_to_1_because_it_is_owned_by_another_user").arg(targetPath);
    }
    const QString permissions = readOnly ? QStringLiteral("r") : QStringLiteral("rw");
    if (!runSetfacl({QStringLiteral("-m"), QStringLiteral("u:%1:%2").arg(QString::fromUtf8(qemuUser->pw_name), permissions), targetPath})) {
        return QCoreApplication::translate("Karton", "failed_to_grant_libvirt_access_to_1").arg(targetPath);
    }
    if (!qemuCanAccess(targetPath, qemuUser->pw_uid, groups, required)) {
        return QCoreApplication::translate("Karton", "libvirt_still_cannot_access_1_after_granting_permissions").arg(targetPath);
    }
    return {};
}

QString Karton::ensureDomainDiskAccess(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return {};
    }
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        return {};
    }
    QDomDocument document;
    QStringList errors;
    if (document.setContent(QString::fromUtf8(xml))) {
        const QDomNodeList disks = document.elementsByTagName(QStringLiteral("disk"));
        for (int i = 0; i < disks.count(); ++i) {
            const QDomElement disk = disks.at(i).toElement();
            const QString device = disk.attribute(QStringLiteral("device"));
            if (device != QStringLiteral("disk") && device != QStringLiteral("cdrom")) {
                continue;
            }
            const QString source = disk.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file"));
            if (source.isEmpty()) {
                continue;
            }
            const bool readOnly = device == QStringLiteral("cdrom") || !disk.firstChildElement(QStringLiteral("readonly")).isNull();
            const QString error = ensureDiskAccess(source, domainConnectionUri(domain), readOnly);
            if (!error.isEmpty()) {
                errors.append(error);
            }
        }
    }
    free(xml);
    return errors.join(QLatin1Char('\n'));
}

QString Karton::checkDirectoryWritable(const QString &path) const
{
    ScopedCallTimer timer("Karton::checkDirectoryWritable");
    if (path.trimmed().isEmpty()) {
        return QCoreApplication::translate("Karton", "the_disk_folder_is_empty");
    }
    const QString directoryPath = QDir::cleanPath(QDir(path).absolutePath());
    if (!QDir().mkpath(directoryPath)) {
        return QCoreApplication::translate("Karton", "failed_to_create_the_disk_folder_1").arg(directoryPath);
    }
    if (::access(directoryPath.toLocal8Bit().constData(), R_OK | W_OK | X_OK) != 0) {
        return QCoreApplication::translate("Karton", "karton_has_no_write_access_to_the_disk_folder_1").arg(directoryPath);
    }
    return {};
}

QString Karton::suggestedDiskPath(const QString &name, const QString &format) const
{
    const QString safeName = name.trimmed().isEmpty() ? QStringLiteral("vm") : name.trimmed();
    return imagesDirectory() + QLatin1Char('/') + safeName + QLatin1Char('.')
        + (format.trimmed().isEmpty() ? QStringLiteral("qcow2") : format.trimmed().toLower());
}

bool Karton::spiceTlsEnabled() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("spice/tls"), true).toBool();
}

void Karton::saveSpiceTlsEnabled(bool enabled)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("spice/tls"), enabled);
}

QString Karton::spiceCaCertificate() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("spice/caCertificate")).toString();
}

void Karton::saveSpiceCaCertificate(const QString &path)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("spice/caCertificate"), path);
}

bool Karton::audioPlaybackEnabled(const Domain *domain) const
{
    if (!domain) {
        return true;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("audio/%1/playback").arg(domain->config()->uuid()), true).toBool();
}

void Karton::saveAudioPlaybackEnabled(const Domain *domain, bool enabled)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("audio/%1/playback").arg(domain->config()->uuid()), enabled);
}

bool Karton::microphoneEnabled(const Domain *domain) const
{
    if (!domain) {
        return false;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("audio/%1/microphone").arg(domain->config()->uuid()), false).toBool();
}

void Karton::saveMicrophoneEnabled(const Domain *domain, bool enabled)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("audio/%1/microphone").arg(domain->config()->uuid()), enabled);
}

bool Karton::guestAgentSeen(const Domain *domain) const
{
    if (!domain) {
        return false;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = guestAgentPrefix(domain);
    if (settings.value(prefix + QStringLiteral("fingerprint")).toString() != guestAgentFingerprint(domain)) {
        return false;
    }
    return settings.value(prefix + QStringLiteral("seen"), false).toBool();
}

void Karton::markGuestAgentSeen(const Domain *domain)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = guestAgentPrefix(domain);
    prepareGuestAgentState(settings, prefix, guestAgentFingerprint(domain));
    if (!settings.value(prefix + QStringLiteral("seen"), false).toBool()) {
        settings.setValue(prefix + QStringLiteral("seen"), true);
    }
}

bool Karton::guestAgentWarningShown(const Domain *domain) const
{
    if (!domain) {
        return false;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = guestAgentPrefix(domain);
    if (settings.value(prefix + QStringLiteral("fingerprint")).toString() != guestAgentFingerprint(domain)) {
        return false;
    }
    return settings.value(prefix + QStringLiteral("warningShown"), false).toBool();
}

void Karton::markGuestAgentWarningShown(const Domain *domain)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = guestAgentPrefix(domain);
    prepareGuestAgentState(settings, prefix, guestAgentFingerprint(domain));
    if (!settings.value(prefix + QStringLiteral("warningShown"), false).toBool()) {
        settings.setValue(prefix + QStringLiteral("warningShown"), true);
    }
}

void Karton::clearGuestAgentState(const Domain *domain)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = guestAgentPrefix(domain);
    settings.remove(prefix + QStringLiteral("seen"));
    settings.remove(prefix + QStringLiteral("warningShown"));
}

QString Karton::language() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString language = settings.value(QStringLiteral("ui/language"), QStringLiteral("auto")).toString();
    if (language == QStringLiteral("auto")) {
        return language;
    }
    return language == QStringLiteral("ru") ? QStringLiteral("ru") : QStringLiteral("en");
}

void Karton::setLanguage(const QString &language)
{
    const QString value = language == QStringLiteral("auto") || language == QStringLiteral("ru") ? language : QStringLiteral("en");
    if (this->language() == value) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ui/language"), value);
    Q_EMIT languageChanged();
}

QString Karton::resolveLanguage(const QString &requested, const QString &systemLocale)
{
    if (requested == QStringLiteral("en") || requested == QStringLiteral("ru")) {
        return requested;
    }
    if (requested != QStringLiteral("auto")) {
        return QStringLiteral("en");
    }
    return systemLocale.left(2) == QStringLiteral("ru") ? QStringLiteral("ru") : QStringLiteral("en");
}

QVariantMap Karton::domainDisplayResolution(const Domain *domain) const
{
    QVariantMap resolution;
    if (!domain) {
        return resolution;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("display/%1/").arg(domain->config()->uuid());
    const int width = settings.value(prefix + QStringLiteral("width"), 0).toInt();
    const int height = settings.value(prefix + QStringLiteral("height"), 0).toInt();
    if (width > 0 && height > 0) {
        resolution.insert(QStringLiteral("width"), width);
        resolution.insert(QStringLiteral("height"), height);
    }
    return resolution;
}

bool Karton::domainDisplayFullscreen(const Domain *domain) const
{
    if (!domain) {
        return false;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("display/%1/fullscreen").arg(domain->config()->uuid()), false).toBool();
}

void Karton::saveDomainDisplayFullscreen(const Domain *domain, bool fullscreen)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("display/%1/").arg(domain->config()->uuid());
    settings.setValue(prefix + QStringLiteral("fullscreen"), fullscreen);
    if (fullscreen) {
        settings.remove(prefix + QStringLiteral("width"));
        settings.remove(prefix + QStringLiteral("height"));
    }
    Q_EMIT displayResolutionChanged(domain);
}

void Karton::saveDomainDisplayResolution(const Domain *domain, int width, int height)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("display/%1/").arg(domain->config()->uuid());
    const int oldWidth = settings.value(prefix + QStringLiteral("width"), 0).toInt();
    const int oldHeight = settings.value(prefix + QStringLiteral("height"), 0).toInt();
    if (width <= 0 || height <= 0) {
        if (oldWidth == 0 && oldHeight == 0) {
            return;
        }
        settings.remove(prefix + QStringLiteral("width"));
        settings.remove(prefix + QStringLiteral("height"));
    } else {
        if (oldWidth == width && oldHeight == height) {
            return;
        }
        settings.setValue(prefix + QStringLiteral("width"), width);
        settings.setValue(prefix + QStringLiteral("height"), height);
    }
    // An open viewer must pick the new mode up without being reopened.
    Q_EMIT displayResolutionChanged(domain);
}
