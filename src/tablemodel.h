/**
 * @licence app begin@
 * Copyright (C) 2011-2012  BMW AG
 *
 * This file is part of COVESA Project Dlt Viewer.
 *
 * Contributions are licensed to the COVESA Alliance under one or more
 * Contribution License Agreements.
 *
 * \copyright
 * This Source Code Form is subject to the terms of the
 * Mozilla Public License, v. 2.0. If a  copy of the MPL was not distributed with
 * this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * \file treemodel.h
 * For further information see http://www.covesa.global/.
 * @licence end@
 */

#ifndef TABLEMODEL_H
#define TABLEMODEL_H

#include <QAbstractItemModel>
#include <QModelIndex>
#include <QVariant>
#include <QMutex>
#include <QStyledItemDelegate>
#include <QHeaderView>
#include <QEvent>
#include <QToolTip>

#include "project.h"
#include "qdltpluginmanager.h"
#include "fieldnames.h"
#include <qdltlrucache.hpp>

#include <optional>
#include <QDateTime>


#define DLT_VIEWER_COLUMN_COUNT FieldNames::Arg0

class TableModel : public QAbstractTableModel
{
Q_OBJECT

public:
    TableModel(const QString &data, QObject *parent = 0);
    ~TableModel();

    QVariant data(const QModelIndex &index, int role) const;
    QVariant headerData(int section, Qt::Orientation orientation,
         int role = Qt::DisplayRole) const;
    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    int columnCount(const QModelIndex &parent = QModelIndex()) const;

    /* pointer to the current loaded file */
    QDltFile *qfile;
    Project *project;
    QDltPluginManager *pluginManager;
    void modelChanged();
    int setMarker(long int lineindex, QColor hlcolor); //used in search functionality
    int setManualMarker(QList<unsigned long int> selectedMarkerRows, QColor hlcolor); //used in mainwindow
    void setForceEmpty(bool emptyForceFlag) { this->emptyForceFlag = emptyForceFlag; }
    void setLoggingOnlyMode(bool loggingOnlyMode) { this->loggingOnlyMode = loggingOnlyMode; }
    void setLastSearchIndex(int idx) {this->lastSearchIndex = idx;}
    QString getToolTipForFields(FieldNames::Fields cn);

    struct OverlayComment
    {
        QString fileName;       // absolute path as opened
        qint64 anchorOffset{-1}; // byte offset in that file (storage header)
        bool after{false};
        QString text;

        // Timestamp fields copied from anchor message (for display)
        QString ecu;
        unsigned int timeSeconds{0};
        unsigned int microseconds{0};
        unsigned int timestamp{0};
        unsigned int sessionId{0};

        qint64 createdUtcMs{0};
    };

    void setOverlayComments(QVector<OverlayComment> comments);
    void clearOverlayComments();

    // Mapping helpers (view-row may include overlay comments)
    std::optional<int> messageFilteredRowForViewRow(int viewRow) const;
    std::optional<int> messageAllIndexForViewRow(int viewRow) const; // QDltFile all-index (getMsgFilterPos)
    int viewRowForFilteredRow(int filteredRow) const;                // view-row of the anchor message row
    bool isCommentRow(int viewRow) const;

private:
    long int lastSearchIndex;
    bool emptyForceFlag;
    bool loggingOnlyMode;

    // cache is used in data()-method to avoid decoding of the same message multiple times
    // key is a message index in the qdltfile; message can fail to decode, in that case value is empty optional
    mutable QDltLruCache<int, std::optional<QDltMsg>> m_cache{1};

    long int searchhit;
    QColor searchBackgroundColor() const;
    QColor searchhit_higlightColor;
    QColor manualMarkerColor;
    QList<unsigned long int> selectedMarkerRows;
    QColor getMsgBackgroundColor(const std::optional<QDltMsg>& msg, int index, long int filterposindex) const;
    bool eventFilter(QObject *obj, QEvent *event);

    void rebuildOverlayMapping();
    int filteredRowForAllIndex(int allIndex) const;

    QVector<OverlayComment> m_overlayComments;
    QVector<int> m_viewRowToFilteredRow;
    QVector<int> m_viewRowToCommentIndex;
    QVector<int> m_filteredRowToViewRow;
};

class HtmlDelegate : public QStyledItemDelegate
{
protected:
    void paint ( QPainter * painter, const QStyleOptionViewItem & option, const QModelIndex & index ) const;
    QSize sizeHint ( const QStyleOptionViewItem & option, const QModelIndex & index ) const;
};

#endif // TABLEMODEL_H
