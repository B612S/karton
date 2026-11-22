// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "viewer/terminalview.h"

#include <algorithm>

#include <QClipboard>
#include <QCursor>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

TerminalView::TerminalView(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setActiveFocusOnTab(true);
    setCursor(QCursor(Qt::IBeamCursor));
    setOpaquePainting(true);
    setAntialiasing(true);

    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    m_font.setPixelSize(m_fontSize);

    m_ansiColors = {
        0xff000000,
        0xffcc0000,
        0xff4e9a06,
        0xffc4a000,
        0xff3465a4,
        0xff75507b,
        0xff06989a,
        0xffd3d7cf,
        0xff555753,
        0xffef2929,
        0xff8ae234,
        0xfffce94f,
        0xff729fcf,
        0xffad7fa8,
        0xff34e2e2,
        0xffeeeeec,
    };

    m_cells = blankScreen();
}

TerminalView::Cell TerminalView::blankCell() const
{
    // Erasing fills the cell with the current background colour, but leaves
    // no glyph attributes behind (an inverse or underline erase must not turn
    // the cleared area into a coloured/underlined block).
    Cell cell;
    cell.fg = m_defaultForeground;
    cell.bg = m_background;
    return cell;
}

TerminalView::Cell &TerminalView::cellAt(int column, int row)
{
    return m_cells[row * m_columns + column];
}

const TerminalView::Cell &TerminalView::cellAt(int column, int row) const
{
    return m_cells[row * m_columns + column];
}

QVector<TerminalView::Cell> TerminalView::blankScreen() const
{
    QVector<Cell> cells(m_columns * m_rows);
    for (Cell &cell : cells) {
        cell = blankCell();
    }
    return cells;
}

void TerminalView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    updateGridGeometry();
    update();
}

void TerminalView::setFontSize(int pixels)
{
    const int size = qBound(6, pixels, 96);
    if (size == m_fontSize) {
        return;
    }
    m_fontSize = size;
    m_font.setPixelSize(m_fontSize);
    updateGridGeometry();
    update();
    Q_EMIT fontSizeChanged();
}

void TerminalView::setFontFamily(const QString &family)
{
    if (m_fontFamily == family) {
        return;
    }
    m_fontFamily = family;
    if (family.isEmpty()) {
        m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        m_font.setStyleHint(QFont::Monospace);
    } else {
        m_font.setFamily(family);
    }
    m_font.setFixedPitch(true);
    m_font.setPixelSize(m_fontSize);
    updateGridGeometry();
    update();
    Q_EMIT fontFamilyChanged();
}

QRgb TerminalView::ansiColor(int index) const
{
    if (index >= 0 && index < m_ansiColors.size()) {
        return m_ansiColors.at(index);
    }
    if (index >= 16 && index <= 231) {
        const int value = index - 16;
        const int r = value / 36;
        const int g = (value / 6) % 6;
        const int b = value % 6;
        const auto component = [](int level) {
            return level == 0 ? 0 : 55 + level * 40;
        };
        return qRgb(component(r), component(g), component(b));
    }
    if (index >= 232 && index <= 255) {
        const int level = 8 + (index - 232) * 10;
        return qRgb(level, level, level);
    }
    return m_defaultForeground;
}

void TerminalView::setColorScheme(const QVariantMap &scheme)
{
    if (scheme.isEmpty()) {
        return;
    }

    const QColor background(scheme.value(QStringLiteral("background")).toString());
    const QColor foreground(scheme.value(QStringLiteral("foreground")).toString());
    QVector<QRgb> ansi = m_ansiColors;
    const QVariantList ansiValues = scheme.value(QStringLiteral("ansi")).toList();
    for (int i = 0; i < qMin(16, ansiValues.size()); ++i) {
        const QColor color(ansiValues.at(i).toString());
        if (color.isValid()) {
            ansi[i] = color.rgb();
        }
    }

    const QRgb newBackground = background.isValid() ? background.rgb() : m_defaultBackground;
    const QRgb newForeground = foreground.isValid() ? foreground.rgb() : m_defaultForeground;
    const QRgb oldBackground = m_defaultBackground;
    const QRgb oldForeground = m_defaultForeground;
    const QVector<QRgb> oldAnsi = m_ansiColors;

    if (newBackground == oldBackground && newForeground == oldForeground && ansi == oldAnsi) {
        m_colorScheme = scheme;
        return;
    }

    const auto remap = [&](QRgb color) {
        if (color == oldForeground) {
            return newForeground;
        }
        if (color == oldBackground) {
            return newBackground;
        }
        const int index = oldAnsi.indexOf(color);
        if (index >= 0 && index < ansi.size()) {
            return ansi.at(index);
        }
        return color;
    };

    for (Cell &cell : m_cells) {
        cell.fg = remap(cell.fg);
        cell.bg = remap(cell.bg);
    }
    for (QVector<Cell> &line : m_scrollback) {
        for (Cell &cell : line) {
            cell.fg = remap(cell.fg);
            cell.bg = remap(cell.bg);
        }
    }
    for (Cell &cell : m_savedCells) {
        cell.fg = remap(cell.fg);
        cell.bg = remap(cell.bg);
    }

    m_defaultBackground = newBackground;
    m_defaultForeground = newForeground;
    m_ansiColors = ansi;
    m_foreground = remap(m_foreground);
    m_background = remap(m_background);
    m_colorScheme = scheme;

    update();
    Q_EMIT colorSchemeChanged();
}

void TerminalView::setCursorShape(const QString &shape)
{
    const QString normalized = shape == QStringLiteral("bar") || shape == QStringLiteral("underline") ? shape : QStringLiteral("block");
    if (m_cursorShape == normalized) {
        return;
    }
    m_cursorShape = normalized;
    update();
    Q_EMIT cursorShapeChanged();
}

void TerminalView::updateGridGeometry()
{
    const qreal availableWidth = width();
    const qreal availableHeight = height();
    if (availableWidth <= 0 || availableHeight <= 0) {
        return;
    }

    // The console uses the configured font size directly; it must not grow to
    // fill the window (that made text huge on large displays). Extra space is
    // used for more columns/rows instead.
    m_font.setPixelSize(m_fontSize);
    QFontMetricsF metrics(m_font);
    m_cellWidth = qMax<qreal>(1.0, metrics.horizontalAdvance(QLatin1Char('M')));
    m_cellHeight = qMax<qreal>(1.0, metrics.height());

    const int columns = qMax(1, static_cast<int>(availableWidth / m_cellWidth));
    const int rows = qMax(1, static_cast<int>(availableHeight / m_cellHeight));
    if (columns != m_columns || rows != m_rows) {
        resizeGrid(columns, rows);
    }

    m_originX = qMax<qreal>(0.0, (availableWidth - m_columns * m_cellWidth) / 2.0);
    m_originY = qMax<qreal>(0.0, (availableHeight - m_rows * m_cellHeight) / 2.0);
}

void TerminalView::resizeGrid(int columns, int rows)
{
    if (columns == m_columns && rows == m_rows) {
        return;
    }
    const int oldColumns = m_columns;
    const int oldRows = m_rows;

    const auto resizeCells = [&](QVector<Cell> &cells) {
        QVector<Cell> resized(columns * rows);
        for (Cell &cell : resized) {
            cell = blankCell();
        }
        const int copyColumns = qMin(columns, oldColumns);
        const int copyRows = qMin(rows, oldRows);
        for (int row = 0; row < copyRows; ++row) {
            for (int column = 0; column < copyColumns; ++column) {
                resized[row * columns + column] = cells[row * oldColumns + column];
            }
        }
        cells = resized;
    };

    resizeCells(m_cells);
    if (m_alternateScreen && !m_savedCells.isEmpty()) {
        resizeCells(m_savedCells);
    }
    for (QVector<Cell> &line : m_scrollback) {
        QVector<Cell> resized(columns);
        for (Cell &cell : resized) {
            cell = blankCell();
        }
        const int copyColumns = qMin(columns, qMin(oldColumns, line.size()));
        for (int column = 0; column < copyColumns; ++column) {
            resized[column] = line[column];
        }
        line = resized;
    }

    m_columns = columns;
    m_rows = rows;
    m_cursorColumn = qBound(0, m_cursorColumn, m_columns - 1);
    m_cursorRow = qBound(0, m_cursorRow, m_rows - 1);
    m_savedCursorColumn = qBound(0, m_savedCursorColumn, m_columns - 1);
    m_savedCursorRow = qBound(0, m_savedCursorRow, m_rows - 1);
    m_scrollOffset = qBound(0, m_scrollOffset, m_scrollback.size());
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_wrapPending = false;
}

void TerminalView::paint(QPainter *painter)
{
    painter->fillRect(boundingRect(), QColor(m_defaultBackground));
    painter->setRenderHint(QPainter::TextAntialiasing, true);
    painter->setFont(m_font);

    if (m_cells.size() != m_columns * m_rows) {
        return;
    }

    const int scrollOffset = qMin(m_scrollOffset, m_scrollback.size());
    for (int row = 0; row < m_rows; ++row) {
        const int screenRow = row - scrollOffset;
        const QVector<Cell> *historyLine = nullptr;
        if (screenRow < 0) {
            historyLine = &m_scrollback.at(m_scrollback.size() + screenRow);
        }
        const qreal y = m_originY + row * m_cellHeight;
        for (int column = 0; column < m_columns; ++column) {
            Cell cell;
            if (historyLine) {
                if (column < historyLine->size()) {
                    cell = historyLine->at(column);
                }
            } else {
                cell = cellAt(column, screenRow);
            }
            QRgb foreground = cell.fg;
            QRgb background = cell.bg;
            if (cell.attributes & Inverse) {
                std::swap(foreground, background);
            }
            if (cell.attributes & Bold) {
                foreground = QColor(foreground).lighter(125).rgb();
            }

            const QRectF rect(m_originX + column * m_cellWidth, y, m_cellWidth, m_cellHeight);
            if (background != m_defaultBackground) {
                painter->fillRect(rect, QColor(background));
            }
            if (cell.ch != QLatin1Char(' ')) {
                painter->setPen(QColor(foreground));
                painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, QString(cell.ch));
            }
            if (cell.attributes & Underline) {
                painter->setPen(QColor(foreground));
                painter->drawLine(QPointF(rect.left(), rect.bottom() - 1), QPointF(rect.right(), rect.bottom() - 1));
            }
        }
    }

    if (scrollOffset == 0 && m_cursorVisible && m_cursorColumn >= 0 && m_cursorColumn < m_columns && m_cursorRow >= 0 && m_cursorRow < m_rows) {
        const QRectF cursorRect(m_originX + m_cursorColumn * m_cellWidth, m_originY + m_cursorRow * m_cellHeight, m_cellWidth, m_cellHeight);
        const Cell &cell = cellAt(m_cursorColumn, m_cursorRow);
        const bool focused = hasActiveFocus();
        const QColor cursorColor = focused ? QColor(cell.fg) : QColor(m_defaultForeground);
        const qreal stroke = qMax(1.0, m_cellHeight / 10.0);
        if (m_cursorShape == QStringLiteral("bar")) {
            painter->fillRect(QRectF(cursorRect.left(), cursorRect.top(), qMax(1.0, m_cellWidth / 8.0), cursorRect.height()), cursorColor);
        } else if (m_cursorShape == QStringLiteral("underline")) {
            painter->fillRect(QRectF(cursorRect.left(), cursorRect.bottom() - stroke, cursorRect.width(), stroke), cursorColor);
        } else if (focused) {
            painter->fillRect(cursorRect, cursorColor);
            if (cell.ch != QLatin1Char(' ')) {
                painter->setPen(QColor(cell.bg));
                painter->drawText(cursorRect, Qt::AlignLeft | Qt::AlignVCenter, QString(cell.ch));
            }
        } else {
            painter->setPen(cursorColor);
            painter->drawRect(cursorRect.adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }
}

void TerminalView::feed(const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    m_pendingText += text;
    scheduleProcess();
}

void TerminalView::scheduleProcess()
{
    if (m_processScheduled) {
        return;
    }
    m_processScheduled = true;
    // Process a bounded chunk per event-loop turn so a burst of console output
    // cannot block painting or input for a whole tick.
    QTimer::singleShot(0, this, &TerminalView::processPending);
}

void TerminalView::processPending()
{
    m_processScheduled = false;
    if (m_pendingText.isEmpty()) {
        return;
    }

    constexpr int chunkSize = 8192;
    const QString chunk = m_pendingText.left(chunkSize);
    m_pendingText.remove(0, chunkSize);
    for (const QChar character : chunk) {
        processCharacter(character);
    }
    if (!m_reply.isEmpty()) {
        const QString response = m_reply;
        m_reply.clear();
        sendText(response);
    }
    update();

    if (!m_pendingText.isEmpty()) {
        scheduleProcess();
    }
}

void TerminalView::clear()
{
    m_pendingText.clear();
    resetTerminal();
    update();
}

void TerminalView::resetTerminal()
{
    m_cells = blankScreen();
    m_savedCells.clear();
    m_alternateScreen = false;
    m_cursorColumn = 0;
    m_cursorRow = 0;
    m_savedCursorColumn = 0;
    m_savedCursorRow = 0;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_wrapPending = false;
    m_autoWrap = true;
    m_cursorVisible = true;
    m_applicationCursorKeys = false;
    m_insertMode = false;
    m_bracketedPaste = false;
    m_foreground = m_defaultForeground;
    m_background = m_defaultBackground;
    m_attributes = 0;
    m_state = State::Ground;
    m_sequence.clear();
    m_scrollback.clear();
    m_scrollOffset = 0;
}

void TerminalView::processCharacter(QChar character)
{
    switch (m_state) {
    case State::Ground:
        if (character == QChar(0x1b)) {
            m_state = State::Escape;
            return;
        }
        switch (character.unicode()) {
        case 0x07: // BEL
            return;
        case 0x08: // BS
            if (m_cursorColumn > 0) {
                --m_cursorColumn;
            }
            m_wrapPending = false;
            return;
        case 0x09: // HT
            m_cursorColumn = tabStop(m_cursorColumn, 1);
            m_wrapPending = false;
            return;
        case 0x0a: // LF
        case 0x0b: // VT
        case 0x0c: // FF
            lineFeed();
            return;
        case 0x0d: // CR
            m_cursorColumn = 0;
            m_wrapPending = false;
            return;
        default:
            break;
        }
        if (character.unicode() < 0x20 || character.unicode() == 0x7f) {
            return;
        }
        putCharacter(character);
        return;

    case State::Escape:
        dispatchEscape(character);
        return;

    case State::Csi:
        if (character == QChar(0x1b)) {
            m_sequence.clear();
            m_state = State::Escape;
            return;
        }
        if (character.unicode() >= 0x40 && character.unicode() <= 0x7e) {
            const QString sequence = m_sequence + character;
            m_sequence.clear();
            m_state = State::Ground;
            dispatchCsi(sequence);
            return;
        }
        if (character.unicode() >= 0x20 && m_sequence.size() < 64) {
            m_sequence += character;
        }
        return;

    case State::Osc:
        if (character == QChar(0x07)) {
            m_state = State::Ground;
        } else if (character == QChar(0x1b)) {
            m_state = State::OscEscape;
        }
        return;

    case State::OscEscape:
        m_state = character == QLatin1Char('\\') ? State::Ground : State::Osc;
        return;

    case State::Dcs:
        if (character == QChar(0x1b)) {
            m_state = State::DcsEscape;
        }
        return;

    case State::DcsEscape:
        m_state = character == QLatin1Char('\\') ? State::Ground : State::Dcs;
        return;

    case State::Charset:
        m_state = State::Ground;
        return;
    }
}

void TerminalView::dispatchEscape(QChar character)
{
    m_state = State::Ground;
    const ushort code = character.unicode();
    switch (code) {
    case '[':
        m_sequence.clear();
        m_state = State::Csi;
        break;
    case ']':
        m_state = State::Osc;
        break;
    case 'P':
    case 'X':
    case '^':
    case '_':
        m_state = State::Dcs;
        break;
    case '7':
        saveCursor();
        break;
    case '8':
        restoreCursor();
        break;
    case 'D':
        lineFeed();
        break;
    case 'E':
        m_cursorColumn = 0;
        lineFeed();
        break;
    case 'M':
        reverseIndex();
        break;
    case 'c':
        resetTerminal();
        break;
    case '(':
    case ')':
    case '*':
    case '+':
    case '-':
    case '.':
    case '/':
    case '#':
        m_state = State::Charset;
        break;
    default:
        break;
    }
}

void TerminalView::dispatchCsi(const QString &sequence)
{
    if (sequence.isEmpty()) {
        return;
    }

    bool privateMode = false;
    QString body = sequence;
    const QChar finalCharacter = body.at(body.size() - 1);
    body.chop(1);
    if (body.startsWith(QLatin1Char('?'))) {
        privateMode = true;
        body.remove(0, 1);
    } else if (body.startsWith(QLatin1Char('>')) || body.startsWith(QLatin1Char('=')) || body.startsWith(QLatin1Char('!'))) {
        return;
    }

    QVector<int> parameters;
    if (!body.isEmpty()) {
        const QStringList parts = body.replace(QLatin1Char(':'), QLatin1Char(';')).split(QLatin1Char(';'));
        for (const QString &part : parts) {
            bool ok = false;
            const int value = part.toInt(&ok);
            parameters.append(ok ? value : -1);
        }
    }
    const auto number = [&parameters](int index, int fallback) {
        if (index < 0 || index >= parameters.size() || parameters.at(index) < 0) {
            return fallback;
        }
        return parameters.at(index);
    };

    switch (finalCharacter.unicode()) {
    case 'A':
        m_cursorRow = qBound(0, m_cursorRow - number(0, 1), m_rows - 1);
        m_wrapPending = false;
        break;
    case 'B':
        m_cursorRow = qBound(0, m_cursorRow + number(0, 1), m_rows - 1);
        m_wrapPending = false;
        break;
    case 'C':
        m_cursorColumn = qBound(0, m_cursorColumn + number(0, 1), m_columns - 1);
        m_wrapPending = false;
        break;
    case 'D':
        m_cursorColumn = qBound(0, m_cursorColumn - number(0, 1), m_columns - 1);
        m_wrapPending = false;
        break;
    case 'E':
        m_cursorColumn = 0;
        m_cursorRow = qBound(0, m_cursorRow + number(0, 1), m_rows - 1);
        m_wrapPending = false;
        break;
    case 'F':
        m_cursorColumn = 0;
        m_cursorRow = qBound(0, m_cursorRow - number(0, 1), m_rows - 1);
        m_wrapPending = false;
        break;
    case 'G':
    case '`':
        m_cursorColumn = qBound(0, number(0, 1) - 1, m_columns - 1);
        m_wrapPending = false;
        break;
    case 'd':
        m_cursorRow = qBound(0, number(0, 1) - 1, m_rows - 1);
        m_wrapPending = false;
        break;
    case 'e':
        m_cursorRow = qBound(0, m_cursorRow + number(0, 1), m_rows - 1);
        m_wrapPending = false;
        break;
    case 'H':
    case 'f':
        m_cursorRow = qBound(0, number(0, 1) - 1, m_rows - 1);
        m_cursorColumn = qBound(0, number(1, 1) - 1, m_columns - 1);
        m_wrapPending = false;
        break;
    case 'J':
        eraseDisplay(number(0, 0));
        break;
    case 'K':
        eraseLine(number(0, 0));
        break;
    case 'L':
        insertLines(number(0, 1));
        break;
    case 'M':
        deleteLines(number(0, 1));
        break;
    case 'P':
        deleteCharacters(number(0, 1));
        break;
    case '@':
        insertCharacters(number(0, 1));
        break;
    case 'X':
        for (int column = m_cursorColumn; column < qMin(m_columns, m_cursorColumn + number(0, 1)); ++column) {
            cellAt(column, m_cursorRow) = blankCell();
        }
        break;
    case 'S':
        scrollUp(number(0, 1));
        break;
    case 'T':
        scrollDown(number(0, 1));
        break;
    case 'r': {
        const int top = number(0, 1) - 1;
        const int bottom = number(1, m_rows) - 1;
        if (top >= 0 && bottom < m_rows && top < bottom) {
            m_scrollTop = top;
            m_scrollBottom = bottom;
            m_cursorColumn = 0;
            m_cursorRow = 0;
            m_wrapPending = false;
        }
        break;
    }
    case 's':
        saveCursor();
        break;
    case 'u':
        restoreCursor();
        break;
    case 'm':
        applySgr(parameters);
        break;
    case 'h':
    case 'l': {
        const bool enabled = finalCharacter == QLatin1Char('h');
        if (privateMode) {
            for (const int parameter : parameters) {
                if (parameter > 0) {
                    setPrivateMode(parameter, enabled);
                }
            }
        } else {
            for (const int parameter : parameters) {
                if (parameter > 0) {
                    setMode(parameter, enabled);
                }
            }
        }
        break;
    }
    case 'n': {
        const int mode = number(0, 0);
        if (mode == 5) {
            reply(QStringLiteral("\x1b[0n"));
        } else if (mode == 6) {
            reply(QStringLiteral("\x1b[%1%2;%3R").arg(privateMode ? QStringLiteral("?") : QString()).arg(m_cursorRow + 1).arg(m_cursorColumn + 1));
        }
        break;
    }
    case 'c':
        reply(QStringLiteral("\x1b[?1;2c"));
        break;
    case 'I':
        for (int i = 0; i < number(0, 1); ++i) {
            m_cursorColumn = tabStop(m_cursorColumn, 1);
        }
        break;
    case 'Z':
        for (int i = 0; i < number(0, 1); ++i) {
            m_cursorColumn = tabStop(m_cursorColumn, -1);
        }
        break;
    case 'b':
        for (int i = 0; i < number(0, 1); ++i) {
            putCharacter(m_lastPrinted);
        }
        break;
    default:
        break;
    }
}

void TerminalView::applySgr(const QVector<int> &parameters)
{
    if (parameters.isEmpty()) {
        m_foreground = m_defaultForeground;
        m_background = m_defaultBackground;
        m_attributes = 0;
        return;
    }

    for (int index = 0; index < parameters.size(); ++index) {
        int parameter = parameters.at(index);
        if (parameter < 0) {
            parameter = 0;
        }
        switch (parameter) {
        case 0:
            m_foreground = m_defaultForeground;
            m_background = m_defaultBackground;
            m_attributes = 0;
            break;
        case 1:
            m_attributes |= Bold;
            break;
        case 4:
            m_attributes |= Underline;
            break;
        case 7:
            m_attributes |= Inverse;
            break;
        case 22:
            m_attributes &= ~Bold;
            break;
        case 24:
            m_attributes &= ~Underline;
            break;
        case 27:
            m_attributes &= ~Inverse;
            break;
        case 39:
            m_foreground = m_defaultForeground;
            break;
        case 49:
            m_background = m_defaultBackground;
            break;
        case 38:
        case 48: {
            if (index + 1 >= parameters.size()) {
                break;
            }
            const int mode = parameters.at(++index);
            QRgb color = m_defaultForeground;
            if (mode == 5 && index + 1 < parameters.size()) {
                color = ansiColor(parameters.at(++index));
            } else if (mode == 2 && index + 3 < parameters.size()) {
                const int r = qBound(0, parameters.at(++index), 255);
                const int g = qBound(0, parameters.at(++index), 255);
                const int b = qBound(0, parameters.at(++index), 255);
                color = qRgb(r, g, b);
                if (parameter == 48) {
                    color |= 0xff000000u;
                }
            }
            if (parameter == 38) {
                m_foreground = color;
            } else {
                m_background = color;
            }
            break;
        }
        default:
            if (parameter >= 30 && parameter <= 37) {
                m_foreground = ansiColor(parameter - 30);
            } else if (parameter >= 40 && parameter <= 47) {
                m_background = ansiColor(parameter - 40);
            } else if (parameter >= 90 && parameter <= 97) {
                m_foreground = ansiColor(parameter - 90 + 8);
            } else if (parameter >= 100 && parameter <= 107) {
                m_background = ansiColor(parameter - 100 + 8);
            }
            break;
        }
    }
}

void TerminalView::setPrivateMode(int mode, bool enabled)
{
    switch (mode) {
    case 1:
        m_applicationCursorKeys = enabled;
        break;
    case 7:
        m_autoWrap = enabled;
        if (!enabled) {
            m_wrapPending = false;
        }
        break;
    case 25:
        m_cursorVisible = enabled;
        break;
    case 47:
    case 1047:
        setAlternateScreen(enabled);
        break;
    case 1049:
        if (enabled) {
            saveCursor();
            setAlternateScreen(true);
        } else {
            setAlternateScreen(false);
            restoreCursor();
        }
        break;
    case 2004:
        m_bracketedPaste = enabled;
        break;
    default:
        break;
    }
}

void TerminalView::setMode(int mode, bool enabled)
{
    if (mode == 4) {
        m_insertMode = enabled;
    }
}

void TerminalView::setAlternateScreen(bool enabled)
{
    if (enabled == m_alternateScreen) {
        return;
    }
    if (enabled) {
        m_savedCells = m_cells;
        m_savedCursorColumn = m_cursorColumn;
        m_savedCursorRow = m_cursorRow;
        m_savedScrollTop = m_scrollTop;
        m_savedScrollBottom = m_scrollBottom;
        m_alternateScreen = true;
        m_cells = blankScreen();
        m_cursorColumn = 0;
        m_cursorRow = 0;
        m_scrollTop = 0;
        m_scrollBottom = m_rows - 1;
    } else {
        m_alternateScreen = false;
        if (m_savedCells.size() == m_columns * m_rows) {
            m_cells = m_savedCells;
            m_cursorColumn = qBound(0, m_savedCursorColumn, m_columns - 1);
            m_cursorRow = qBound(0, m_savedCursorRow, m_rows - 1);
            m_scrollTop = qBound(0, m_savedScrollTop, m_rows - 1);
            m_scrollBottom = qBound(m_scrollTop, m_savedScrollBottom, m_rows - 1);
        } else {
            m_cells = blankScreen();
            m_cursorColumn = 0;
            m_cursorRow = 0;
            m_scrollTop = 0;
            m_scrollBottom = m_rows - 1;
        }
        m_savedCells.clear();
    }
    m_wrapPending = false;
    m_scrollOffset = 0;
}

void TerminalView::putCharacter(QChar character)
{
    m_lastPrinted = character;
    if (m_wrapPending) {
        m_wrapPending = false;
        if (m_autoWrap) {
            m_cursorColumn = 0;
            lineFeed();
        }
    }

    if (m_insertMode) {
        insertCharacters(1);
    }

    m_cursorColumn = qBound(0, m_cursorColumn, m_columns - 1);
    m_cursorRow = qBound(0, m_cursorRow, m_rows - 1);
    Cell &cell = cellAt(m_cursorColumn, m_cursorRow);
    cell.ch = character;
    cell.fg = m_foreground;
    cell.bg = m_background;
    cell.attributes = m_attributes;

    if (m_cursorColumn == m_columns - 1) {
        if (m_autoWrap) {
            m_wrapPending = true;
        }
    } else {
        ++m_cursorColumn;
    }
}

void TerminalView::lineFeed()
{
    m_wrapPending = false;
    if (m_cursorRow == m_scrollBottom) {
        scrollUp(1);
    } else if (m_cursorRow < m_rows - 1) {
        ++m_cursorRow;
    }
}

void TerminalView::reverseIndex()
{
    m_wrapPending = false;
    if (m_cursorRow == m_scrollTop) {
        scrollDown(1);
    } else if (m_cursorRow > 0) {
        --m_cursorRow;
    }
}

void TerminalView::scrollUp(int count)
{
    const int regionRows = m_scrollBottom - m_scrollTop + 1;
    count = qBound(1, count, regionRows);

    // Keep lines that scroll off the normal screen for the scrollback buffer.
    // Alternate-screen content (full-screen apps) is deliberately not saved.
    if (m_scrollTop == 0 && m_scrollBottom == m_rows - 1 && !m_alternateScreen) {
        for (int i = 0; i < count; ++i) {
            QVector<Cell> line(m_columns);
            for (int column = 0; column < m_columns; ++column) {
                line[column] = cellAt(column, i);
            }
            m_scrollback.append(line);
        }
        const int excess = m_scrollback.size() - MaximumScrollbackLines;
        if (excess > 0) {
            m_scrollback.remove(0, excess);
        }
    }

    for (int row = m_scrollTop; row <= m_scrollBottom - count; ++row) {
        for (int column = 0; column < m_columns; ++column) {
            cellAt(column, row) = cellAt(column, row + count);
        }
    }
    for (int row = m_scrollBottom - count + 1; row <= m_scrollBottom; ++row) {
        for (int column = 0; column < m_columns; ++column) {
            cellAt(column, row) = blankCell();
        }
    }
}

void TerminalView::scrollDown(int count)
{
    const int regionRows = m_scrollBottom - m_scrollTop + 1;
    count = qBound(1, count, regionRows);
    for (int row = m_scrollBottom; row >= m_scrollTop + count; --row) {
        for (int column = 0; column < m_columns; ++column) {
            cellAt(column, row) = cellAt(column, row - count);
        }
    }
    for (int row = m_scrollTop; row < m_scrollTop + count; ++row) {
        for (int column = 0; column < m_columns; ++column) {
            cellAt(column, row) = blankCell();
        }
    }
}

void TerminalView::eraseLine(int mode)
{
    int start = 0;
    int end = m_columns - 1;
    if (mode == 0) {
        start = m_cursorColumn;
    } else if (mode == 1) {
        end = m_cursorColumn;
    }
    for (int column = start; column <= end; ++column) {
        cellAt(column, m_cursorRow) = blankCell();
    }
}

void TerminalView::eraseDisplay(int mode)
{
    if (mode == 0) {
        for (int column = m_cursorColumn; column < m_columns; ++column) {
            cellAt(column, m_cursorRow) = blankCell();
        }
        for (int row = m_cursorRow + 1; row < m_rows; ++row) {
            for (int column = 0; column < m_columns; ++column) {
                cellAt(column, row) = blankCell();
            }
        }
    } else if (mode == 1) {
        for (int row = 0; row < m_cursorRow; ++row) {
            for (int column = 0; column < m_columns; ++column) {
                cellAt(column, row) = blankCell();
            }
        }
        for (int column = 0; column <= m_cursorColumn; ++column) {
            cellAt(column, m_cursorRow) = blankCell();
        }
    } else if (mode == 3) {
        m_scrollback.clear();
        m_scrollOffset = 0;
    } else {
        for (int row = 0; row < m_rows; ++row) {
            for (int column = 0; column < m_columns; ++column) {
                cellAt(column, row) = blankCell();
            }
        }
    }
}

void TerminalView::insertCharacters(int count)
{
    count = qBound(1, count, m_columns - m_cursorColumn);
    for (int column = m_columns - 1; column >= m_cursorColumn + count; --column) {
        cellAt(column, m_cursorRow) = cellAt(column - count, m_cursorRow);
    }
    for (int column = m_cursorColumn; column < m_cursorColumn + count; ++column) {
        cellAt(column, m_cursorRow) = blankCell();
    }
}

void TerminalView::deleteCharacters(int count)
{
    count = qBound(1, count, m_columns - m_cursorColumn);
    for (int column = m_cursorColumn; column < m_columns - count; ++column) {
        cellAt(column, m_cursorRow) = cellAt(column + count, m_cursorRow);
    }
    for (int column = m_columns - count; column < m_columns; ++column) {
        cellAt(column, m_cursorRow) = blankCell();
    }
}

void TerminalView::insertLines(int count)
{
    if (m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom) {
        return;
    }
    const int savedTop = m_scrollTop;
    m_scrollTop = m_cursorRow;
    scrollDown(count);
    m_scrollTop = savedTop;
}

void TerminalView::deleteLines(int count)
{
    if (m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom) {
        return;
    }
    const int savedTop = m_scrollTop;
    m_scrollTop = m_cursorRow;
    scrollUp(count);
    m_scrollTop = savedTop;
}

void TerminalView::saveCursor()
{
    m_savedCursorColumn = m_cursorColumn;
    m_savedCursorRow = m_cursorRow;
}

void TerminalView::restoreCursor()
{
    m_cursorColumn = qBound(0, m_savedCursorColumn, m_columns - 1);
    m_cursorRow = qBound(0, m_savedCursorRow, m_rows - 1);
    m_wrapPending = false;
}

int TerminalView::tabStop(int column, int direction) const
{
    if (direction >= 0) {
        return qMin(m_columns - 1, ((column / 8) + 1) * 8);
    }
    return qMax(0, ((column - 1) / 8) * 8);
}

void TerminalView::reply(const QString &text)
{
    m_reply += text;
}

void TerminalView::sendText(const QString &text)
{
    if (!text.isEmpty()) {
        Q_EMIT dataToSend(text);
    }
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    const int key = event->key();
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool control = modifiers & Qt::ControlModifier;
    const bool shift = modifiers & Qt::ShiftModifier;
    const bool alt = modifiers & Qt::AltModifier;

    // Typing while reading the scrollback returns to the live screen, like a
    // normal terminal. Shift+PgUp/PgDn keep navigating the history.
    const bool scrollingKey = shift && (key == Qt::Key_PageUp || key == Qt::Key_PageDown);
    if (!scrollingKey && m_scrollOffset != 0) {
        m_scrollOffset = 0;
        update();
    }

    if (control && shift && key == Qt::Key_V) {
        pasteFromClipboard();
        event->accept();
        return;
    }
    if (control && !alt) {
        int code = -1;
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            code = key - Qt::Key_A + 1;
        } else {
            switch (key) {
            case Qt::Key_Space:
            case Qt::Key_At:
                code = 0;
                break;
            case Qt::Key_BracketLeft:
                code = 0x1b;
                break;
            case Qt::Key_Backslash:
                code = 0x1c;
                break;
            case Qt::Key_BracketRight:
                code = 0x1d;
                break;
            case Qt::Key_AsciiCircum:
                code = 0x1e;
                break;
            case Qt::Key_Underscore:
                code = 0x1f;
                break;
            default:
                break;
            }
        }
        if (code >= 0) {
            sendText(QString(QChar(code)));
            event->accept();
            return;
        }
    }

    if (alt && !control && !event->text().isEmpty()) {
        sendText(QString(QChar(0x1b)) + event->text());
        event->accept();
        return;
    }

    const QString applicationCursor = m_applicationCursorKeys ? QStringLiteral("\x1bO") : QStringLiteral("\x1b[");
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        sendText(QStringLiteral("\r"));
        event->accept();
        return;
    case Qt::Key_Backspace:
        sendText(QStringLiteral("\x7f"));
        event->accept();
        return;
    case Qt::Key_Tab:
        sendText(QStringLiteral("\t"));
        event->accept();
        return;
    case Qt::Key_Backtab:
        sendText(QStringLiteral("\x1b[Z"));
        event->accept();
        return;
    case Qt::Key_Escape:
        sendText(QStringLiteral("\x1b"));
        event->accept();
        return;
    case Qt::Key_Up:
        sendText(applicationCursor + QStringLiteral("A"));
        event->accept();
        return;
    case Qt::Key_Down:
        sendText(applicationCursor + QStringLiteral("B"));
        event->accept();
        return;
    case Qt::Key_Right:
        sendText(applicationCursor + QStringLiteral("C"));
        event->accept();
        return;
    case Qt::Key_Left:
        sendText(applicationCursor + QStringLiteral("D"));
        event->accept();
        return;
    case Qt::Key_Home:
        sendText(applicationCursor + QStringLiteral("H"));
        event->accept();
        return;
    case Qt::Key_End:
        sendText(applicationCursor + QStringLiteral("F"));
        event->accept();
        return;
    case Qt::Key_Insert:
        sendText(QStringLiteral("\x1b[2~"));
        event->accept();
        return;
    case Qt::Key_Delete:
        sendText(QStringLiteral("\x1b[3~"));
        event->accept();
        return;
    case Qt::Key_PageUp:
        if (shift && !m_alternateScreen) {
            m_scrollOffset = qMin<int>(m_scrollback.size(), m_scrollOffset + m_rows);
            update();
        } else {
            sendText(QStringLiteral("\x1b[5~"));
        }
        event->accept();
        return;
    case Qt::Key_PageDown:
        if (shift && !m_alternateScreen) {
            m_scrollOffset = qMax(0, m_scrollOffset - m_rows);
            update();
        } else {
            sendText(QStringLiteral("\x1b[6~"));
        }
        event->accept();
        return;
    case Qt::Key_F1:
        sendText(QStringLiteral("\x1bOP"));
        event->accept();
        return;
    case Qt::Key_F2:
        sendText(QStringLiteral("\x1bOQ"));
        event->accept();
        return;
    case Qt::Key_F3:
        sendText(QStringLiteral("\x1bOR"));
        event->accept();
        return;
    case Qt::Key_F4:
        sendText(QStringLiteral("\x1bOS"));
        event->accept();
        return;
    case Qt::Key_F5:
        sendText(QStringLiteral("\x1b[15~"));
        event->accept();
        return;
    case Qt::Key_F6:
        sendText(QStringLiteral("\x1b[17~"));
        event->accept();
        return;
    case Qt::Key_F7:
        sendText(QStringLiteral("\x1b[18~"));
        event->accept();
        return;
    case Qt::Key_F8:
        sendText(QStringLiteral("\x1b[19~"));
        event->accept();
        return;
    case Qt::Key_F9:
        sendText(QStringLiteral("\x1b[20~"));
        event->accept();
        return;
    case Qt::Key_F10:
        sendText(QStringLiteral("\x1b[21~"));
        event->accept();
        return;
    case Qt::Key_F11:
        sendText(QStringLiteral("\x1b[23~"));
        event->accept();
        return;
    case Qt::Key_F12:
        sendText(QStringLiteral("\x1b[24~"));
        event->accept();
        return;
    default:
        break;
    }

    const QString text = event->text();
    if (!text.isEmpty() && text.at(0).unicode() >= 0x20) {
        sendText(text);
        event->accept();
        return;
    }

    QQuickPaintedItem::keyPressEvent(event);
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus();
    event->accept();
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    if (m_alternateScreen || m_scrollback.isEmpty()) {
        event->accept();
        return;
    }
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        QQuickPaintedItem::wheelEvent(event);
        return;
    }
    const int lines = qMax(1, qAbs(delta) / 40);
    if (delta > 0) {
        m_scrollOffset = qMin<int>(m_scrollback.size(), m_scrollOffset + lines);
    } else {
        m_scrollOffset = qMax(0, m_scrollOffset - lines);
    }
    update();
    event->accept();
}

void TerminalView::focusInEvent(QFocusEvent *event)
{
    QQuickPaintedItem::focusInEvent(event);
    update();
}

void TerminalView::focusOutEvent(QFocusEvent *event)
{
    QQuickPaintedItem::focusOutEvent(event);
    update();
}

void TerminalView::pasteFromClipboard()
{
    QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty()) {
        return;
    }
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
    text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    if (m_bracketedPaste) {
        text = QStringLiteral("\x1b[200~") + text + QStringLiteral("\x1b[201~");
    }
    sendText(text);
}
