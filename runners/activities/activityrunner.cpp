/*
 *   Copyright (C) 2011 Aaron Seigo <aseigo@kde.org>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License version 2 as
 *   published by the Free Software Foundation
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "activityrunner.h"

#include <QDebug>
#include <QIcon>
#include <klocalizedstring.h>

K_EXPORT_PLASMA_RUNNER(activities, ActivityRunner)

ActivityRunner::ActivityRunner(QObject *parent, const QVariantList &args)
    : Plasma::AbstractRunner(parent, args),
      m_activities(nullptr),
      m_consumer(nullptr),
      m_keywordi18n(i18nc("KRunner keyword", "activity")),
      m_keyword(QStringLiteral("activity")),
      m_enabled(false)
{
    setObjectName(QStringLiteral("Activities"));
    setIgnoredTypes(Plasma::RunnerContext::Directory | Plasma::RunnerContext::File |
                    Plasma::RunnerContext::NetworkLocation | Plasma::RunnerContext::Help);

    connect(this, &Plasma::AbstractRunner::prepare, this, &ActivityRunner::prep);
    connect(this, &Plasma::AbstractRunner::teardown, this, &ActivityRunner::down);

    qRegisterMetaType<KActivities::Consumer::ServiceStatus>();

    serviceStatusChanged(KActivities::Consumer::Running);
}

void ActivityRunner::prep()
{
    if (!m_activities) {
        m_activities = new KActivities::Controller(this);
        m_consumer = new KActivities::Consumer(this);
        connect(m_consumer, &KActivities::Consumer::serviceStatusChanged,
                this, &ActivityRunner::serviceStatusChanged);
        serviceStatusChanged(m_activities->serviceStatus());
    }
}

void ActivityRunner::down()
{
    delete m_activities;
    m_activities = nullptr;
}

void ActivityRunner::serviceStatusChanged(KActivities::Consumer::ServiceStatus status)
{
    const bool active = status != KActivities::Consumer::NotRunning;
    if (m_enabled == active) {
        return;
    }

    m_enabled = active;
    QList<Plasma::RunnerSyntax> syntaxes;
    if (m_enabled) {
        setDefaultSyntax(Plasma::RunnerSyntax(m_keywordi18n, i18n("Lists all activities currently available to be run.")));
        addSyntax(Plasma::RunnerSyntax(i18nc("KRunner keyword", "activity :q:"), i18n("Switches to activity :q:.")));
    }
}

ActivityRunner::~ActivityRunner()
{
}

void ActivityRunner::match(Plasma::RunnerContext &context)
{
    if (!m_enabled) {
        return;
    }

    const QString term = context.query().trimmed();
    bool list = false;
    QString name;

    if (term.startsWith(m_keywordi18n, Qt::CaseInsensitive)) {
        if (term.size() == m_keywordi18n.size()) {
            list = true;
        } else {
            name = term.right(term.size() - m_keywordi18n.size()).trimmed();
            list = name.isEmpty();
        }
    } else if (term.startsWith(m_keyword, Qt::CaseInsensitive)) {
        if (term.size() == m_keyword.size()) {
            list = true;
        } else {
            name = term.right(term.size() - m_keyword.size()).trimmed();
            list = name.isEmpty();
        }
    } else if (context.singleRunnerQueryMode()) {
        name = term;
    } else {
        return;
    }

    QList<Plasma::QueryMatch> matches;
    QStringList activities = m_consumer->activities();
    std::sort(activities.begin(), activities.end());

    const QString current = m_activities->currentActivity();

    if (!context.isValid()) {
        return;
    }

    if (list) {
        foreach (const QString &activity, activities) {
            if (current == activity) {
                continue;
            }

            KActivities::Info info(activity);
            addMatch(info, matches);

            if (!context.isValid()) {
                return;
            }
        }
    } else {
        foreach (const QString &activity, activities) {
            if (current == activity) {
                continue;
            }

            KActivities::Info info(activity);
            if (info.name().startsWith(name, Qt::CaseInsensitive)) {
                addMatch(info, matches);
            }

            if (!context.isValid()) {
                return;
            }
        }
    }

    context.addMatches(matches);
}

void ActivityRunner::addMatch(const KActivities::Info &activity, QList<Plasma::QueryMatch> &matches)
{
    Plasma::QueryMatch match(this);
    match.setData(activity.id());
    match.setType(Plasma::QueryMatch::ExactMatch);
    match.setIconName(activity.icon().isEmpty() ? QStringLiteral("activities") : activity.icon());
    match.setText(i18n("Switch to \"%1\"", activity.name()));
    match.setRelevance(0.7 + ((activity.state() == KActivities::Info::Running ||
                               activity.state() == KActivities::Info::Starting) ? 0.1 : 0));
    matches << match;
}

void ActivityRunner::run(const Plasma::RunnerContext &context, const Plasma::QueryMatch &match)
{
    Q_UNUSED(context)

    if (!m_enabled || !m_activities) {
        return;
    }

    m_activities->setCurrentActivity(match.data().toString());
}

#include "activityrunner.moc"
