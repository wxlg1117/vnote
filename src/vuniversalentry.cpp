#include "vuniversalentry.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QDebug>
#include <QHideEvent>
#include <QShowEvent>
#include <QPaintEvent>
#include <QKeyEvent>
#include <QListWidgetItem>
#include <QStyleOption>
#include <QPainter>
#include <QTimer>
#include <QAtomicInt>

#include "vmetawordlineedit.h"
#include "utils/vutils.h"
#include "vlistwidget.h"
#include "vpalette.h"

#define MINIMUM_WIDTH 200

#define CMD_EDIT_INTERVAL 500

extern VPalette *g_palette;

VUniversalEntryContainer::VUniversalEntryContainer(QWidget *p_parent)
    : QWidget(p_parent),
      m_widget(NULL)
{
    m_layout = new QVBoxLayout();
    m_layout->setContentsMargins(0, 0, 0, 0);

    setLayout(m_layout);
}

void VUniversalEntryContainer::clear()
{
    if (m_widget) {
        m_layout->removeWidget(m_widget);
        m_widget->hide();
        m_widget = NULL;
    }

    adjustSizeByWidget();
}

void VUniversalEntryContainer::setWidget(QWidget *p_widget)
{
    if (m_widget != p_widget) {
        clear();
        m_widget = p_widget;
        m_layout->addWidget(m_widget);
        m_widget->show();
    }

    adjustSizeByWidget();
}

void VUniversalEntryContainer::adjustSizeByWidget()
{
    updateGeometry();
}

QSize VUniversalEntryContainer::sizeHint() const
{
    if (m_widget) {
        return m_widget->sizeHint();
    } else {
        return QWidget::sizeHint();
    }
}


VUniversalEntry::VUniversalEntry(QWidget *p_parent)
    : QWidget(p_parent),
      m_availableRect(0, 0, MINIMUM_WIDTH, MINIMUM_WIDTH),
      m_lastEntry(NULL),
      m_inQueue(0),
      m_pendingCommand(false)
{
    m_minimumWidth = MINIMUM_WIDTH * VUtils::calculateScaleFactor() + 0.5;

    m_cmdTimer = new QTimer(this);
    m_cmdTimer->setSingleShot(true);
    m_cmdTimer->setInterval(CMD_EDIT_INTERVAL);
    connect(m_cmdTimer, &QTimer::timeout,
            this, [this]() {
                processCommand();
            });

    setupUI();

    m_infoWidget = new VListWidget(this);
    m_infoWidget->setFitContent(true);
    m_container->setWidget(m_infoWidget);

    m_cmdStyleSheet = m_cmdEdit->styleSheet();
}

void VUniversalEntry::setupUI()
{
    m_cmdEdit = new VMetaWordLineEdit(this);
    m_cmdEdit->setPlaceholderText(tr("Welcome to Universal Entry"));
    connect(m_cmdEdit, &VMetaWordLineEdit::textEdited,
            this, [this]() {
                m_cmdTimer->stop();
                m_cmdTimer->start();
            });

    m_container = new VUniversalEntryContainer(this);

    QVBoxLayout *mainLayout = new QVBoxLayout();
    mainLayout->addWidget(m_cmdEdit);
    mainLayout->addWidget(m_container);

    mainLayout->setContentsMargins(1, 1, 1, 1);
    mainLayout->setSpacing(0);

    setLayout(mainLayout);

    setMinimumWidth(m_minimumWidth);
}

void VUniversalEntry::hideEvent(QHideEvent *p_event)
{
    QWidget::hideEvent(p_event);
    if (m_lastEntry) {
        m_lastEntry->m_entry->entryHidden(m_lastEntry->m_id);
    }

    emit exited();
}

void VUniversalEntry::showEvent(QShowEvent *p_event)
{
    QWidget::showEvent(p_event);

    // Fix Chinese input method issue.
    activateWindow();

    m_cmdEdit->setFocus();
}

void VUniversalEntry::setAvailableRect(const QRect &p_rect)
{
    m_availableRect = p_rect;

    if (m_availableRect.width() < m_minimumWidth) {
        m_availableRect.setWidth(m_minimumWidth);
    }

    setMaximumSize(m_availableRect.size());
}

void VUniversalEntry::registerEntry(QChar p_key, IUniversalEntry *p_entry, int p_id)
{
    Q_ASSERT(!m_entries.contains(p_key));

    p_entry->setParent(this);
    p_entry->setWidgetParent(this);
    connect(p_entry, &IUniversalEntry::widgetUpdated,
            this, [this]() {
                m_container->adjustSizeByWidget();
                adjustSize();
            });
    connect(p_entry, &IUniversalEntry::stateUpdated,
            this, &VUniversalEntry::updateState);
    connect(p_entry, &IUniversalEntry::requestHideUniversalEntry,
            this, &VUniversalEntry::hide);

    m_entries.insert(p_key, Entry(p_entry, p_id));
    m_infoWidget->addItem(QString("%1: %2").arg(p_key)
                                           .arg(p_entry->description(p_id)));
    m_infoWidget->updateGeometry();
}

void VUniversalEntry::processCommand()
{
    if (!m_inQueue.testAndSetRelaxed(0, 1)) {
        // There is already a job in queue.
        qDebug() << "already a job in queue, pend a new job and ask to stop";
        m_pendingCommand = true;
        if (m_lastEntry) {
            m_lastEntry->m_entry->askToStop(m_lastEntry->m_id);
        }

        return;
    }

redo:
    QString cmd = m_cmdEdit->getEvaluatedText();
    processCommand(cmd);
    if (m_pendingCommand && cmd != m_cmdEdit->getEvaluatedText()) {
        // Handle pending job.
        qDebug() << "handle pending job" << cmd;
        m_pendingCommand = false;
        goto redo;
    }

    m_inQueue.store(0);
}

void VUniversalEntry::processCommand(const QString &p_cmd)
{
    if (p_cmd.isEmpty()) {
        clear();
        return;
    }

    auto it = m_entries.find(p_cmd[0]);
    if (it == m_entries.end()) {
        clear();
        return;
    }

    const Entry &entry = it.value();
    if (m_lastEntry && m_lastEntry != &entry) {
        m_lastEntry->m_entry->clear(m_lastEntry->m_id);
    }

    m_lastEntry = &entry;
    m_container->setWidget(entry.m_entry->widget(entry.m_id));
    adjustSize();
    entry.m_entry->processCommand(entry.m_id, p_cmd.mid(1));
}

void VUniversalEntry::clear()
{
    if (m_lastEntry) {
        m_lastEntry->m_entry->clear(m_lastEntry->m_id);
        m_lastEntry = NULL;
    }

    m_container->setWidget(m_infoWidget);
    adjustSize();
}

void VUniversalEntry::keyPressEvent(QKeyEvent *p_event)
{
    int modifiers = p_event->modifiers();
    bool forward = true;
    switch (p_event->key()) {
    case Qt::Key_BracketLeft:
        if (VUtils::isControlModifierForVim(modifiers)) {
            // Hide.
            hide();
            return;
        }

        break;

    // Up/Down Ctrl+K/J to navigate to next item.
    case Qt::Key_Up:
        forward = false;
        V_FALLTHROUGH;
    case Qt::Key_Down:
        if (m_lastEntry) {
            m_lastEntry->m_entry->selectNextItem(m_lastEntry->m_id, forward);
            return;
        }

        break;

    case Qt::Key_K:
        forward = false;
        V_FALLTHROUGH;
    case Qt::Key_J:
        if (m_lastEntry && VUtils::isControlModifierForVim(modifiers)) {
            m_lastEntry->m_entry->selectNextItem(m_lastEntry->m_id, forward);
            return;
        }

        break;

    case Qt::Key_Enter:
        // Fall through.
        V_FALLTHROUGH;
    case Qt::Key_Return:
    {
        if (m_lastEntry) {
            m_lastEntry->m_entry->activate(m_lastEntry->m_id);
            return;
        }

        break;
    }

    case Qt::Key_E:
        if (VUtils::isControlModifierForVim(modifiers)) {
            // Ctrl+E to eliminate input except the command key.
            QString cmd = m_cmdEdit->getEvaluatedText();
            if (!cmd.isEmpty()) {
                m_cmdEdit->setText(cmd.left(1));
                m_cmdTimer->stop();
                processCommand();
                return;
            }
        }

        break;

    case Qt::Key_D:
        if (VUtils::isControlModifierForVim(modifiers)) {
            // Ctrl+D to cancel current command.
            m_pendingCommand = false;
            if (m_lastEntry) {
                m_lastEntry->m_entry->askToStop(m_lastEntry->m_id);
            }
            return;
        }

        break;

    default:
        break;
    }

    QWidget::keyPressEvent(p_event);
}

void VUniversalEntry::paintEvent(QPaintEvent *p_event)
{
    QStyleOption opt;
    opt.init(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);

    QWidget::paintEvent(p_event);
}

void VUniversalEntry::updateState(IUniversalEntry::State p_state)
{
    QString fg;
    switch (p_state) {
    case IUniversalEntry::Busy:
        fg = g_palette->color("ue_cmd_busy_border");
        break;

    case IUniversalEntry::Fail:
        fg = g_palette->color("ue_cmd_fail_border");
        break;

    default:
        break;
    }

    if (fg.isEmpty()) {
        m_cmdEdit->setStyleSheet(m_cmdStyleSheet);
    } else {
        m_cmdEdit->setStyleSheet(QString("border-color: %1;").arg(fg));
    }
}
