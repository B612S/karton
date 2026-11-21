// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "util/xmlhighlighter.h"

#include <QColor>
#include <QRegularExpression>
#include <QTextDocument>

XmlSyntaxHighlighter::XmlSyntaxHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    m_tagFormat.setForeground(QColor(QStringLiteral("#2f7fd1")));
    m_attributeFormat.setForeground(QColor(QStringLiteral("#d28b26")));
    m_valueFormat.setForeground(QColor(QStringLiteral("#4d9a5b")));
    m_commentFormat.setForeground(QColor(QStringLiteral("#7f8c8d")));
    m_declarationFormat.setForeground(QColor(QStringLiteral("#9b59b6")));
    m_declarationFormat.setFontItalic(true);
}

void XmlSyntaxHighlighter::setTagColor(const QColor &color)
{
    if (m_tagFormat.foreground().color() == color) {
        return;
    }
    m_tagFormat.setForeground(color);
    rehighlight();
}

void XmlSyntaxHighlighter::setAttributeColor(const QColor &color)
{
    if (m_attributeFormat.foreground().color() == color) {
        return;
    }
    m_attributeFormat.setForeground(color);
    rehighlight();
}

void XmlSyntaxHighlighter::setValueColor(const QColor &color)
{
    if (m_valueFormat.foreground().color() == color) {
        return;
    }
    m_valueFormat.setForeground(color);
    rehighlight();
}

void XmlSyntaxHighlighter::setCommentColor(const QColor &color)
{
    if (m_commentFormat.foreground().color() == color) {
        return;
    }
    m_commentFormat.setForeground(color);
    rehighlight();
}

void XmlSyntaxHighlighter::setDeclarationColor(const QColor &color)
{
    if (m_declarationFormat.foreground().color() == color) {
        return;
    }
    m_declarationFormat.setForeground(color);
    rehighlight();
}

void XmlSyntaxHighlighter::highlightBlock(const QString &text)
{
    // Comments are highlighted first and take precedence over tags.
    const QRegularExpression comment(QStringLiteral("<!--[\\s\\S]*?-->"));
    auto it = comment.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_commentFormat);
    }

    const QRegularExpression declaration(QStringLiteral("<\\?[^>]*\\?>|<![^>]*>"));
    it = declaration.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_declarationFormat);
    }

    const QRegularExpression tag(QStringLiteral("</?[A-Za-z_][\\w:.-]*"));
    it = tag.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_tagFormat);
    }

    const QRegularExpression attribute(QStringLiteral("[A-Za-z_][\\w:.-]*(?=\\s*=)"));
    it = attribute.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_attributeFormat);
    }

    const QRegularExpression value(QStringLiteral("\"[^\"]*\"|'[^']*'"));
    it = value.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_valueFormat);
    }
}

XmlHighlighter::XmlHighlighter(QObject *parent)
    : QObject(parent)
    , m_tagColor(QStringLiteral("#2f7fd1"))
    , m_attributeColor(QStringLiteral("#d28b26"))
    , m_valueColor(QStringLiteral("#4d9a5b"))
    , m_commentColor(QStringLiteral("#7f8c8d"))
    , m_declarationColor(QStringLiteral("#9b59b6"))
{
}

QQuickTextDocument *XmlHighlighter::textDocument() const
{
    return m_textDocument;
}

void XmlHighlighter::setTextDocument(QQuickTextDocument *document)
{
    if (m_textDocument == document) {
        return;
    }
    if (m_highlighter) {
        delete m_highlighter;
        m_highlighter = nullptr;
    }
    m_textDocument = document;
    if (m_textDocument) {
        m_highlighter = new XmlSyntaxHighlighter(m_textDocument->textDocument());
        applyColors();
        m_highlighter->rehighlight();
    }
    Q_EMIT textDocumentChanged();
}

QColor XmlHighlighter::tagColor() const
{
    return m_tagColor;
}

void XmlHighlighter::setTagColor(const QColor &color)
{
    if (m_tagColor == color) {
        return;
    }
    m_tagColor = color;
    applyColors();
    Q_EMIT tagColorChanged();
}

QColor XmlHighlighter::attributeColor() const
{
    return m_attributeColor;
}

void XmlHighlighter::setAttributeColor(const QColor &color)
{
    if (m_attributeColor == color) {
        return;
    }
    m_attributeColor = color;
    applyColors();
    Q_EMIT attributeColorChanged();
}

QColor XmlHighlighter::valueColor() const
{
    return m_valueColor;
}

void XmlHighlighter::setValueColor(const QColor &color)
{
    if (m_valueColor == color) {
        return;
    }
    m_valueColor = color;
    applyColors();
    Q_EMIT valueColorChanged();
}

QColor XmlHighlighter::commentColor() const
{
    return m_commentColor;
}

void XmlHighlighter::setCommentColor(const QColor &color)
{
    if (m_commentColor == color) {
        return;
    }
    m_commentColor = color;
    applyColors();
    Q_EMIT commentColorChanged();
}

QColor XmlHighlighter::declarationColor() const
{
    return m_declarationColor;
}

void XmlHighlighter::setDeclarationColor(const QColor &color)
{
    if (m_declarationColor == color) {
        return;
    }
    m_declarationColor = color;
    applyColors();
    Q_EMIT declarationColorChanged();
}

void XmlHighlighter::applyColors()
{
    if (!m_highlighter) {
        return;
    }
    m_highlighter->setTagColor(m_tagColor);
    m_highlighter->setAttributeColor(m_attributeColor);
    m_highlighter->setValueColor(m_valueColor);
    m_highlighter->setCommentColor(m_commentColor);
    m_highlighter->setDeclarationColor(m_declarationColor);
}
