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
 * \file treemodel.cpp
 * For further information see http://www.covesa.global/.
 * @licence end@
 */

#include <QtGui>
#include <QApplication>
#include <qmessagebox.h>

#include <algorithm>

#include "tablemodel.h"
#include "fieldnames.h"
#include "dltuiutils.h"
#include "dlt_protocol.h"
#include "qdltoptmanager.h"

TableModel::TableModel(const QString & /*data*/, QObject *parent)
     : QAbstractTableModel(parent)
 {
     qfile = NULL;
     project = NULL;
     pluginManager = NULL;
     lastSearchIndex = -1;
     emptyForceFlag = false;
     loggingOnlyMode = false;
     searchhit = -1;

     m_viewRowToFilteredRow.clear();
     m_viewRowToCommentIndex.clear();
     m_filteredRowToViewRow.clear();
 }

 TableModel::~TableModel()
 {

 }

 int TableModel::columnCount(const QModelIndex & /*parent*/) const
 {
     return DLT_VIEWER_COLUMN_COUNT+project->settings->showArguments;
 }


 QVariant TableModel::data(const QModelIndex &index, int role) const
 {
     if (!index.isValid())
     {
         return QVariant();
     }

     if (index.row() < 0 || index.row() >= rowCount())
     {
         return QVariant();
     }

     if (loggingOnlyMode) {
         if ((role == Qt::DisplayRole) && (index.column() == FieldNames::Payload))
            return QString("Logging only Mode! Disable in Project Settings!");
         else
            return QVariant();
     }

     if (role == Qt::TextAlignmentRole)
     {
         return FieldNames::getColumnAlignment((FieldNames::Fields)index.column(),project->settings);
     }

     const int viewRow = index.row();
     if(isCommentRow(viewRow))
     {
         const int commentIdx = (viewRow >= 0 && viewRow < m_viewRowToCommentIndex.size()) ? m_viewRowToCommentIndex[viewRow] : -1;
         if(commentIdx < 0 || commentIdx >= m_overlayComments.size())
             return QVariant();
         const auto& c = m_overlayComments[commentIdx];

         if(role == Qt::ForegroundRole)
             return QBrush(DltUiUtils::optimalTextColor(QColor(0,255,0)));
         if(role == Qt::BackgroundRole)
             return QBrush(QColor(0,255,0));
         if(role == Qt::ToolTipRole)
             return c.text;

         if(role != Qt::DisplayRole)
             return QVariant();

         switch(index.column())
         {
         case FieldNames::Index:
             return QString();
         case FieldNames::Time:
         {
             const QDateTime dt = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(c.timeSeconds), Qt::UTC);
             return QString("%1.%2").arg(dt.toString("yyyy/MM/dd hh:mm:ss")).arg(c.microseconds, 6, 10, QLatin1Char('0'));
         }
         case FieldNames::TimeStamp:
             return QString("%1.%2").arg(c.timestamp/10000).arg(c.timestamp%10000,4,10,QLatin1Char('0'));
         case FieldNames::Counter:
             return QString();
         case FieldNames::EcuId:
             return c.ecu;
         case FieldNames::AppId:
             return QStringLiteral("USER");
         case FieldNames::ContextId:
             return QStringLiteral("CMNT");
         case FieldNames::SessionId:
             return QString("%1").arg(c.sessionId);
         case FieldNames::Type:
             return QStringLiteral("log");
         case FieldNames::Subtype:
             return QStringLiteral("info");
         case FieldNames::Mode:
             return QStringLiteral("verbose");
         case FieldNames::ArgCount:
             return QStringLiteral("1");
         case FieldNames::Payload:
             return c.text;
         default:
             return QVariant();
         }
     }

     const int filteredRow = (viewRow >= 0 && viewRow < m_viewRowToFilteredRow.size()) ? m_viewRowToFilteredRow[viewRow] : viewRow;
     if(filteredRow < 0 || filteredRow >= qfile->sizeFilter())
         return QVariant();
     long int filterposindex = qfile->getMsgFilterPos(filteredRow);

     std::optional<QDltMsg> msg;
     if (m_cache.exists(filteredRow)) {
         msg = m_cache.get(filteredRow);
     } else {
         QDltMsg omsg;
         if (bool success = qfile->getMsg(filterposindex, omsg); success) {
            msg = std::make_optional(omsg);
            if (QDltSettingsManager::getInstance()->value("startup/pluginsEnabled", true).toBool()) {
                pluginManager->decodeMsg(*msg, !QDltOptManager::getInstance()->issilentMode());
            }
         }

         m_cache.put(filteredRow, msg);
     }

     if (role == Qt::DisplayRole)
     {
       if (!msg.has_value())
       {
         if(index.column() == FieldNames::Index)
         {
             return QString("%1").arg(qfile->getMsgFilterPos(filteredRow));
         }
         else if(index.column() == FieldNames::Payload)
         {
             qDebug() << "Corrupted message at index" << index.row();
             return QString("!!CORRUPTED MESSAGE!!");
         }
         return QVariant();
       }

         QString visu_data;
         switch(index.column())
         {
         case FieldNames::Index:
             /* display index */
             return QString("%L1").arg(qfile->getMsgFilterPos(filteredRow));
         case FieldNames::Time:
             if( project->settings->automaticTimeSettings == 0 )
                return QString("%1.%2").arg(msg->getGmTimeWithOffsetString(project->settings->utcOffset,project->settings->dst)).arg(msg->getMicroseconds(),6,10,QLatin1Char('0'));
             else
                return QString("%1.%2").arg(msg->getTimeString()).arg(msg->getMicroseconds(),6,10,QLatin1Char('0'));
         case FieldNames::TimeStamp:
             return QString("%1.%2").arg(msg->getTimestamp()/10000).arg(msg->getTimestamp()%10000,4,10,QLatin1Char('0'));
         case FieldNames::Counter:
             return QString("%1").arg(msg->getMessageCounter());
         case FieldNames::EcuId:
             return msg->getEcuid();
         case FieldNames::AppId:
             switch(project->settings->showApIdDesc)
             {
             case 0:
                return msg->getApid();
                 break;
             case 1:
                   for(int num = 0; num < project->ecu->topLevelItemCount (); num++)
                    {
                     EcuItem *ecuitem = (EcuItem*)project->ecu->topLevelItem(num);
                     for(int numapp = 0; numapp < ecuitem->childCount(); numapp++)
                     {
                         ApplicationItem * appitem = (ApplicationItem *) ecuitem->child(numapp);
                         if(appitem->id == msg->getApid() && !appitem->description.isEmpty())
                         {
                            return appitem->description;
                         }
                     }
                    }
                   return QString("Apid: %1 (No description)").arg(msg->getApid());
                 break;
              default:
                 return msg->getApid();
             }
         case FieldNames::ContextId:
             switch(project->settings->showCtIdDesc)
             {
             case 0:
                 return msg->getCtid();
                 break;
             case 1:
                   for(int num = 0; num < project->ecu->topLevelItemCount (); num++)
                    {
                     EcuItem *ecuitem = (EcuItem*)project->ecu->topLevelItem(num);
                     for(int numapp = 0; numapp < ecuitem->childCount(); numapp++)
                     {
                         ApplicationItem * appitem = (ApplicationItem *) ecuitem->child(numapp);
                         for(int numcontext = 0; numcontext < appitem->childCount(); numcontext++)
                         {
                             ContextItem * conitem = (ContextItem *) appitem->child(numcontext);

                             if(appitem->id == msg->getApid() && conitem->id == msg->getCtid()
                                     && !conitem->description.isEmpty())
                             {
                                return conitem->description;
                             }
                         }
                     }
                    }
                   return  QString("Ctid: %1 (No description)").arg(msg->getCtid());
                 break;
              default:
                 return msg->getCtid();
             }
         case FieldNames::SessionId:
             switch(project->settings->showSessionName){
             case 0:
                 return QString("%1").arg(msg->getSessionid());
                 break;
             case 1:
                 if(!msg->getSessionName().isEmpty())
                 {
                    return msg->getSessionName();
                 }
                else
                 {
                    return QString("%1").arg(msg->getSessionid());
                 }
                 break;
              default:
                 return QString("%1").arg(msg->getSessionid());
             }
         case FieldNames::Type:
             return msg->getTypeString();
         case FieldNames::Subtype:
             return msg->getSubtypeString();
         case FieldNames::Mode:
             return msg->getModeString();
         case FieldNames::ArgCount:
             return QString("%1").arg(msg->getNumberOfArguments());
         case FieldNames::Payload:
             /* display payload */
             visu_data = msg->toStringPayload().simplified().remove(QChar::Null);
             if(qfile) qfile->applyRegExString(*msg,visu_data);
             /*if((QDltSettingsManager::getInstance()->value("startup/filtersEnabled", true).toBool()))
             {
                 for(int num = 0; num < project->filter->topLevelItemCount (); num++) {
                     FilterItem *item = (FilterItem*)project->filter->topLevelItem(num);
                     if(item->checkState(0) == Qt::Checked && item->filter.enableRegexSearchReplace) {
                         visu_data.replace(QRegularExpression(item->filter.regex_search), item->filter.regex_replace);
                     }
                 }
             }*/

             /* limit size of string to 1000 characters to speed up scrolling */
             if(visu_data.size()>1000)
             {
                visu_data = visu_data.mid(0,1000);
             }

             return visu_data;
         case FieldNames::MessageId:
             return QString::asprintf(project->settings->msgIdFormat.toUtf8(), msg->getMessageId());
         default:
             if (index.column()>=FieldNames::Arg0)
             {
                 int col=index.column()-FieldNames::Arg0; //arguments a zero based
                 QDltArgument arg;
                 if (msg->getArgument(col,arg))
                 {
                     return arg.toString();
                 }
                 else
                 {
                  return QString(" - ");
                 }

             }
         }
     }

     if ( role == Qt::ForegroundRole )
     {
         /* Calculate background color and find optimal foreground color */
         return QBrush(DltUiUtils::optimalTextColor(getMsgBackgroundColor(msg, filteredRow,filterposindex)));
     }

     if ( role == Qt::BackgroundRole )
     {
         /* Calculate background color */
         return QBrush(getMsgBackgroundColor(msg, filteredRow,filterposindex));
     }

    if ( role == Qt::ToolTipRole )
    {
        if (!msg.has_value())
        {
            return QString("!!CORRUPTED MESSAGE!!");
        }

        QString visu_data = msg->toStringPayload().simplified().remove(QChar::Null);
        if((QDltSettingsManager::getInstance()->value("startup/filtersEnabled", true).toBool()))
        {
            for(int num = 0; num < project->filter->topLevelItemCount (); num++) {
                FilterItem *item = (FilterItem*)project->filter->topLevelItem(num);
                if(item->checkState(0) == Qt::Checked && item->filter.enableRegexSearchReplace) {
                    visu_data.replace(QRegularExpression(item->filter.regex_search), item->filter.regex_replace);
                }
            }
        }

        return visu_data;
    }

     return QVariant();
 }

QVariant TableModel::headerData(int section, Qt::Orientation orientation,
                                int role) const
{    
    if (orientation == Qt::Horizontal)
    {
        switch (role)
        {
        case Qt::DisplayRole:
            return FieldNames::getName((FieldNames::Fields)section, project->settings);
        case Qt::TextAlignmentRole:
            {
            /*switch(section)
                {
                 //case FieldNames::Payload: return QVariant(Qt::AlignRight  | Qt::AlignVCenter);
                default:
                }*/
            return FieldNames::getColumnAlignment((FieldNames::Fields)section,project->settings);
            }
         default:
            break;
        }
    }

    return QVariant();
}

 int TableModel::rowCount(const QModelIndex & /*parent*/) const
 {
     if(true == emptyForceFlag)
         return 0;
     else if(true == loggingOnlyMode)
         return 1;
     else
     {
         if(!qfile)
             return 0;
         if(!m_viewRowToFilteredRow.isEmpty() && m_viewRowToFilteredRow.size() == m_viewRowToCommentIndex.size())
             return m_viewRowToFilteredRow.size();
         return qfile->sizeFilter();
     }
 }

 void TableModel::modelChanged()
 {
     if(true == emptyForceFlag)
     {
         index(0, 1);
         index(qfile->sizeFilter()-1, 0);
         index(qfile->sizeFilter()-1, columnCount() - 1);
     }
     else
     {
         index(0, 1);
         index(0, 0);
         index(0, columnCount() - 1);
     }

     /* last search index must be deleted because model changed */
     lastSearchIndex = -1;

     rebuildOverlayMapping();

     emit(layoutChanged());
 }

int TableModel::setManualMarker(QList<unsigned long int> selectedRows, QColor hlcolor) //used in mainwindow
{
manualMarkerColor = hlcolor;
this->selectedMarkerRows = selectedRows;
return 0;
}

int TableModel::setMarker(long int lineindex, QColor hlcolor)
{
  searchhit_higlightColor = hlcolor;
  searchhit = lineindex;
  return 0;
}

QColor TableModel::searchBackgroundColor() const
{
    QString color = QDltSettingsManager::getInstance()->value("other/searchResultColor", QString("#00AAFF")).toString();
    QColor hlColor(color);
    return hlColor;
}

void HtmlDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem optionV4 = option;
    initStyleOption(&optionV4, index);

    QStyle *style = optionV4.widget? optionV4.widget->style() : QApplication::style();

    QTextDocument doc;
    doc.setHtml(optionV4.text);

    /// Painting item without text
    optionV4.text = QString();
    style->drawControl(QStyle::CE_ItemViewItem, &optionV4, painter);

    QAbstractTextDocumentLayout::PaintContext ctx;

    // Highlighting text if item is selected
    if (optionV4.state & QStyle::State_Selected)
        ctx.palette.setColor(QPalette::Text, optionV4.palette.color(QPalette::Active, QPalette::HighlightedText));

    QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &optionV4);
    painter->save();
    painter->translate(textRect.topLeft());
    painter->setClipRect(textRect.translated(-textRect.topLeft()));
    doc.documentLayout()->draw(painter, ctx);
    painter->restore();

}

QSize HtmlDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem optionV4 = option;
    initStyleOption(&optionV4, index);

    QTextDocument doc;
    doc.setHtml(optionV4.text);
    doc.setTextWidth(optionV4.rect.width());
    return QSize(doc.idealWidth(), doc.size().height());
}

QColor TableModel::getMsgBackgroundColor(const std::optional<QDltMsg>& msg, int index, long int filterposindex) const
{
    /* first check manual markers with highest priority */
    if ( selectedMarkerRows.contains(filterposindex) )
    {
      return manualMarkerColor;
    }

    if (!msg.has_value())
    {
        return QColor(1, 2, 3);
    }

    /* get check marker color */
    if(QColor color = qfile->checkMarker(*msg); color.isValid())
    {
       /* Valid marker found, use background color as defined in marker */
       return color;
    }

    // User comments written into the DLT file
    if(msg->getApid() == QStringLiteral("USER") && msg->getCtid() == QStringLiteral("CMNT"))
    {
        return QColor(0,255,0);
    }

     if(lastSearchIndex != -1 && filterposindex == qfile->getMsgFilterPos(lastSearchIndex))
    {
        return searchBackgroundColor();
    }
    if ( searchhit > -1 && searchhit == index )
    {
      return searchhit_higlightColor;
    }
    if(project->settings->autoMarkFatalError && ( msg->getSubtypeString() == "error" || msg->getSubtypeString() == "fatal") )
    {
       /* If automark error is enabled, set red as background color */
       return QColor(255,0,0);
    }
    if(project->settings->autoMarkWarn && msg->getSubtypeString() == "warn")
    {
       /* If automark warning is enabled, set red as background color */
       return QColor(255,255,0);
    }
    if(project->settings->autoMarkMarker && msg->getType()==QDltMsg::DltTypeControl &&
        msg->getSubtype()==QDltMsg::DltControlResponse && msg->getCtrlServiceId() == DLT_SERVICE_ID_MARKER)
    {
        /* If automark marker is enabled, set green as background color */
       return QColor(0,255,0);
    }


    /* default return white background color */
    QColor brushColor = QColor(255,255,255);

    if (QDltSettingsManager::UI_Colour::UI_Dark == QDltSettingsManager::getInstance()->uiColour)
    {
        brushColor = QColor(31,31,31);
    }

    return brushColor; // this is the default background color
}

QString TableModel::getToolTipForFields(FieldNames::Fields cn)
{
    switch(cn){
    case(FieldNames::Time): return "Detailed representation of time and date when the log entry was recorded";
    case(FieldNames::TimeStamp): return "Displays the time when the log message was generated";
    default: return "";
    }
}

bool TableModel::eventFilter(QObject *obj, QEvent *event) {
    if(event->type() == QEvent::Enter || event->type() == QEvent::Leave){
        QHeaderView *header = qobject_cast<QHeaderView*>(obj);
        if(header){
            QPoint pos = QCursor::pos();
            pos = header->mapFromGlobal(pos);
            header->setMouseTracking(true);
            int index = header->logicalIndexAt(pos);
            QString toolTipText;
            switch(index){
            case 1:
                toolTipText = getToolTipForFields(FieldNames::Time);
                break;
            case 2:
                toolTipText = getToolTipForFields(FieldNames::TimeStamp);
                break;
            default:
                toolTipText = "";
            }
            if(!toolTipText.isEmpty()){
                QToolTip::showText(QCursor::pos(), toolTipText, header);
            } else {
                QToolTip::hideText();
            }
        }
    }
    return QObject::eventFilter(obj,event);
}

std::optional<int> TableModel::messageFilteredRowForViewRow(int viewRow) const
{
    if(!qfile)
    {
        return std::nullopt;
    }
    if(viewRow < 0 || viewRow >= rowCount())
    {
        return std::nullopt;
    }
    if(isCommentRow(viewRow))
        return std::nullopt;
    if(viewRow >= 0 && viewRow < m_viewRowToFilteredRow.size())
        return m_viewRowToFilteredRow[viewRow];
    if(viewRow < qfile->sizeFilter())
        return viewRow;
    return std::nullopt;
}

std::optional<int> TableModel::messageAllIndexForViewRow(int viewRow) const
{
    if(!qfile)
    {
        return std::nullopt;
    }
    if(viewRow < 0 || viewRow >= rowCount())
    {
        return std::nullopt;
    }
    if(isCommentRow(viewRow))
        return std::nullopt;
    const auto frOpt = messageFilteredRowForViewRow(viewRow);
    if(!frOpt.has_value())
        return std::nullopt;
    return qfile->getMsgFilterPos(*frOpt);
}

int TableModel::viewRowForFilteredRow(int filteredRow) const
{
    if(!qfile)
    {
        return -1;
    }
    if(filteredRow < 0 || filteredRow >= qfile->sizeFilter())
    {
        return -1;
    }
    if(filteredRow >= 0 && filteredRow < m_filteredRowToViewRow.size() && m_filteredRowToViewRow[filteredRow] >= 0)
        return m_filteredRowToViewRow[filteredRow];
    return filteredRow;
}

bool TableModel::isCommentRow(int viewRow) const
{
    if(viewRow < 0 || viewRow >= m_viewRowToCommentIndex.size())
        return false;
    return m_viewRowToCommentIndex[viewRow] >= 0;
}

void TableModel::setOverlayComments(QVector<OverlayComment> comments)
{
    m_overlayComments = std::move(comments);
    rebuildOverlayMapping();
}

void TableModel::clearOverlayComments()
{
    m_overlayComments.clear();
    rebuildOverlayMapping();
}

int TableModel::filteredRowForAllIndex(int allIndex) const
{
    if(!qfile)
        return -1;
    if(allIndex < 0)
        return -1;

    int lo = 0;
    int hi = qfile->sizeFilter() - 1;
    while(lo <= hi)
    {
        const int mid = lo + (hi - lo) / 2;
        const int pos = qfile->getMsgFilterPos(mid);
        if(pos == allIndex)
            return mid;
        if(pos < allIndex)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

void TableModel::rebuildOverlayMapping()
{
    m_viewRowToFilteredRow.clear();
    m_viewRowToCommentIndex.clear();
    m_filteredRowToViewRow.clear();

    if(!qfile)
        return;

    const int filteredCount = qfile->sizeFilter();
    m_filteredRowToViewRow.resize(filteredCount);
    std::fill(m_filteredRowToViewRow.begin(), m_filteredRowToViewRow.end(), -1);

    QVector<QVector<int>> before(filteredCount);
    QVector<QVector<int>> after(filteredCount);

    for(int ci = 0; ci < m_overlayComments.size(); ++ci)
    {
        const auto& c = m_overlayComments[ci];
        int fileNum = -1;
        for(int i = 0; i < qfile->getNumberOfFiles(); ++i)
        {
            if(qfile->getFileName(i) == c.fileName)
            {
                fileNum = i;
                break;
            }
        }
        if(fileNum < 0)
            continue;

        int allIndex = -1;
        if(!qfile->getMsgIndexFromOffset(fileNum, c.anchorOffset, allIndex))
            continue;

        const int fr = filteredRowForAllIndex(allIndex);
        if(fr < 0 || fr >= filteredCount)
            continue;

        if(c.after)
            after[fr].append(ci);
        else
            before[fr].append(ci);
    }

    auto stableSortByCreated = [this](QVector<int>& vec) {
        std::stable_sort(vec.begin(), vec.end(), [this](int a, int b) {
            return m_overlayComments[a].createdUtcMs < m_overlayComments[b].createdUtcMs;
        });
    };

    for(int fr = 0; fr < filteredCount; ++fr)
    {
        stableSortByCreated(before[fr]);
        stableSortByCreated(after[fr]);
    }

    int viewRow = 0;
    m_viewRowToFilteredRow.reserve(filteredCount + m_overlayComments.size());
    m_viewRowToCommentIndex.reserve(filteredCount + m_overlayComments.size());

    for(int fr = 0; fr < filteredCount; ++fr)
    {
        for(const int ci : before[fr])
        {
            m_viewRowToFilteredRow.append(-1);
            m_viewRowToCommentIndex.append(ci);
            ++viewRow;
        }

        m_filteredRowToViewRow[fr] = viewRow;
        m_viewRowToFilteredRow.append(fr);
        m_viewRowToCommentIndex.append(-1);
        ++viewRow;

        for(const int ci : after[fr])
        {
            m_viewRowToFilteredRow.append(-1);
            m_viewRowToCommentIndex.append(ci);
            ++viewRow;
        }
    }
}
