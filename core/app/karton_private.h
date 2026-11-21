// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <QByteArray>
#include <QDir>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVector>

#include "domain/domain.h"

// Small helpers shared by the Karton implementation files. They used to live
// in an anonymous namespace in karton.cpp; the implementation is now split by
// responsibility so they need internal linkage across several translation
// units. This header is private to the application.
namespace KartonCommon
{
inline QString kartonConfigPath()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/karton");
    QDir().mkpath(directory);
    return directory + QStringLiteral("/karton.conf");
}

inline bool isValidDomainName(const QString &name)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9._-]+$"));
    return expression.match(name).hasMatch();
}

// The user-visible VM order is stored per libvirt connection: the same VM list
// can look different over qemu:///session and qemu:///system. The URI is
// Base64-encoded because QSettings treats '/' as a group separator.
inline QString domainOrderSettingsKey(const QString &uri)
{
    const QByteArray encoded = uri.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QStringLiteral("domainOrder/%1").arg(QString::fromLatin1(encoded));
}

inline QStringList storedDomainOrder(const QString &uri)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(domainOrderSettingsKey(uri)).toStringList();
}

inline void saveDomainOrder(const QVector<Domain *> &domains, const QString &uri)
{
    QStringList order;
    order.reserve(domains.size());
    for (const Domain *domain : domains) {
        if (domain) {
            order.append(domain->config()->uuid());
        }
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(domainOrderSettingsKey(uri), order);
}
} // namespace KartonCommon
