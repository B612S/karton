// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <QColor>
#include <QFont>
#include <QQuickPaintedItem>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class QKeyEvent;
class QPainter;
class QWheelEvent;

// A compact VT100/xterm-compatible terminal used for the libvirt serial
// console. It intentionally implements only the terminal features needed by a
// Linux text console: cursor addressing, scrolling, erase, SGR colors, the
// alternate screen and the common key sequences.
class TerminalView : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontFamilyChanged)
    Q_PROPERTY(QVariantMap colorScheme READ colorScheme WRITE setColorScheme NOTIFY colorSchemeChanged)
    Q_PROPERTY(QString cursorShape READ cursorShape WRITE setCursorShape NOTIFY cursorShapeChanged)

public:
    explicit TerminalView(QQuickItem *parent = nullptr);

    void paint(QPainter *painter) override;

    Q_INVOKABLE void feed(const QString &text);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void pasteFromClipboard();

    int fontSize() const
    {
        return m_fontSize;
    }
    void setFontSize(int pixels);
    QString fontFamily() const
    {
        return m_fontFamily;
    }
    void setFontFamily(const QString &family);
    QVariantMap colorScheme() const
    {
        return m_colorScheme;
    }
    void setColorScheme(const QVariantMap &scheme);
    QString cursorShape() const
    {
        return m_cursorShape;
    }
    void setCursorShape(const QString &shape);

Q_SIGNALS:
    void dataToSend(const QString &text);
    void fontSizeChanged();
    void fontFamilyChanged();
    void colorSchemeChanged();
    void cursorShapeChanged();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void scheduleProcess();
    void processPending();

    struct Cell {
        QChar ch = QLatin1Char(' ');
        QRgb fg = 0xffd0d0d0;
        QRgb bg = 0xff101010;
        quint8 attributes = 0;
    };

    enum Attribute : quint8 {
        Bold = 0x01,
        Underline = 0x02,
        Inverse = 0x04,
    };

    enum class State {
        Ground,
        Escape,
        Csi,
        Osc,
        OscEscape,
        Dcs,
        DcsEscape,
        Charset,
    };

    Cell blankCell() const;
    QRgb ansiColor(int index) const;
    Cell &cellAt(int column, int row);
    const Cell &cellAt(int column, int row) const;
    QVector<Cell> blankScreen() const;

    void updateGridGeometry();
    void resizeGrid(int columns, int rows);
    int tabStop(int column, int direction) const;

    void resetTerminal();
    void setAlternateScreen(bool enabled);

    void processCharacter(QChar character);
    void dispatchEscape(QChar character);
    void dispatchCsi(const QString &sequence);
    void applySgr(const QVector<int> &parameters);
    void setPrivateMode(int mode, bool enabled);
    void setMode(int mode, bool enabled);

    void putCharacter(QChar character);
    void lineFeed();
    void reverseIndex();
    void scrollUp(int count);
    void scrollDown(int count);
    void eraseLine(int mode);
    void eraseDisplay(int mode);
    void insertCharacters(int count);
    void deleteCharacters(int count);
    void insertLines(int count);
    void deleteLines(int count);
    void saveCursor();
    void restoreCursor();

    void sendText(const QString &text);
    void reply(const QString &text);

    QVector<Cell> m_cells;
    QVector<Cell> m_savedCells;
    QVector<QVector<Cell>> m_scrollback;
    int m_scrollOffset = 0;
    static constexpr int MaximumScrollbackLines = 2000;
    int m_columns = 80;
    int m_rows = 24;
    int m_cursorColumn = 0;
    int m_cursorRow = 0;
    int m_savedCursorColumn = 0;
    int m_savedCursorRow = 0;
    int m_scrollTop = 0;
    int m_scrollBottom = 23;
    int m_savedScrollTop = 0;
    int m_savedScrollBottom = 23;
    bool m_wrapPending = false;
    bool m_autoWrap = true;
    bool m_cursorVisible = true;
    bool m_applicationCursorKeys = false;
    bool m_insertMode = false;
    bool m_bracketedPaste = false;
    bool m_alternateScreen = false;

    QRgb m_defaultForeground = 0xffd0d0d0;
    QRgb m_defaultBackground = 0xff101010;
    QVector<QRgb> m_ansiColors;
    QVariantMap m_colorScheme;
    QRgb m_foreground = m_defaultForeground;
    QRgb m_background = m_defaultBackground;
    quint8 m_attributes = 0;
    QChar m_lastPrinted = QLatin1Char(' ');

    State m_state = State::Ground;
    QString m_sequence;
    QString m_reply;
    QString m_pendingText;
    bool m_processScheduled = false;

    QFont m_font;
    int m_fontSize = 12;
    QString m_fontFamily;
    QString m_cursorShape = QStringLiteral("block");
    qreal m_cellWidth = 8;
    qreal m_cellHeight = 16;
    qreal m_originX = 0;
    qreal m_originY = 0;
};
