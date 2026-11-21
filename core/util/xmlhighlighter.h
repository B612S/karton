// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class XmlSyntaxHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    explicit XmlSyntaxHighlighter(QTextDocument *document);

    void setTagColor(const QColor &color);
    void setAttributeColor(const QColor &color);
    void setValueColor(const QColor &color);
    void setCommentColor(const QColor &color);
    void setDeclarationColor(const QColor &color);

protected:
    void highlightBlock(const QString &text) override;

private:
    QTextCharFormat m_tagFormat;
    QTextCharFormat m_attributeFormat;
    QTextCharFormat m_valueFormat;
    QTextCharFormat m_commentFormat;
    QTextCharFormat m_declarationFormat;
};

class XmlHighlighter : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *textDocument READ textDocument WRITE setTextDocument NOTIFY textDocumentChanged)
    Q_PROPERTY(QColor tagColor READ tagColor WRITE setTagColor NOTIFY tagColorChanged)
    Q_PROPERTY(QColor attributeColor READ attributeColor WRITE setAttributeColor NOTIFY attributeColorChanged)
    Q_PROPERTY(QColor valueColor READ valueColor WRITE setValueColor NOTIFY valueColorChanged)
    Q_PROPERTY(QColor commentColor READ commentColor WRITE setCommentColor NOTIFY commentColorChanged)
    Q_PROPERTY(QColor declarationColor READ declarationColor WRITE setDeclarationColor NOTIFY declarationColorChanged)

public:
    explicit XmlHighlighter(QObject *parent = nullptr);

    QQuickTextDocument *textDocument() const;
    void setTextDocument(QQuickTextDocument *document);

    QColor tagColor() const;
    void setTagColor(const QColor &color);
    QColor attributeColor() const;
    void setAttributeColor(const QColor &color);
    QColor valueColor() const;
    void setValueColor(const QColor &color);
    QColor commentColor() const;
    void setCommentColor(const QColor &color);
    QColor declarationColor() const;
    void setDeclarationColor(const QColor &color);

Q_SIGNALS:
    void textDocumentChanged();
    void tagColorChanged();
    void attributeColorChanged();
    void valueColorChanged();
    void commentColorChanged();
    void declarationColorChanged();

private:
    void applyColors();

    QPointer<QQuickTextDocument> m_textDocument;
    QPointer<XmlSyntaxHighlighter> m_highlighter;
    QColor m_tagColor;
    QColor m_attributeColor;
    QColor m_valueColor;
    QColor m_commentColor;
    QColor m_declarationColor;
};
