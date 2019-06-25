/*  This file is part of the KDE libraries and the KTextEditor project.
 *
 *  Copyright (C) 2019 Nibaldo González S. <nibgonz@gmail.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 * ---------------------------------------------------------------------
 *  NOTE: The KateModeMenuListData::SearchLine class is based on
 *  KListWidgetSearchLine, by Scott Wheeler <wheeler@kde.org> and
 *  Gustavo Sverzut Barbieri <gsbarbieri@users.sourceforge.net>.
 *  See: https://api.kde.org/frameworks/kitemviews/html/classKListWidgetSearchLine.html
 *
 *  TODO: Add keyboard shortcut to show the menu. Put the menu in
 *  the center of the window if the status bar is hidden.
 *  See: KateModeMenuList::showEvent()
 */

#ifndef KATEMODEMENULIST_H
#define KATEMODEMENULIST_H

#include <QMenu>
#include <QListView>
#include <QStandardItemModel>
#include <QLineEdit>
#include <QScrollBar>
#include <QPushButton>
#include <QGridLayout>
#include <QString>
#include <QLabel>
#include <QIcon>
#include <QKeyEvent>

#include "katemodemanager.h"

namespace KTextEditor { class DocumentPrivate; }

namespace KateModeMenuListData { class ListView; class ListItem; class SearchLine; }

/**
 * Class of menu to select the
 * syntax highlighting language (mode menu).
 * Provides a menu with a scrollable list plus search bar.
 *
 * This is an alternative to the classic mode menu of the KateModeMenu class.
 *
 * @see KateModeManager, KateFileType, KateModeMenu
 */
class KateModeMenuList : public QMenu
{
   Q_OBJECT

public:
    /**
     * Alignment with respect to the trigger button.
     * "Default" is the normal alignment (left alignment in Left-to-right layouts).
     * "Inverse" uses right alignment in Left-to-right layouts and left
     * alignment in Right-to-left layouts (used in some languages).
     * "Left" and "Right" forces the alignment.
     * @see setButton(), QWidget::layoutDirection(), Qt::LayoutDirection
     */
    enum AlignmentButton {
        Default,
        Inverse,
        Left,
        Right
    };
    /**
     * Search bar position, above or below the list.
     */
    enum SearchBarPosition {
        Top,
        Bottom
    };
    /**
     * Defines where the list will scroll after clearing the search or changing the view.
     * @see setAutoScroll(), autoScroll()
     */
    enum AutoScroll {
        ScrollToSelectedItem,
        ScrollToTop
    };

    /**
     * @param searchBarPos Search bar position, can be top or bottom.
     * @see SearchBarPosition
     */
    KateModeMenuList(const SearchBarPosition searchBarPos = Bottom) : QMenu()
    {
        init(searchBarPos);
    }
    KateModeMenuList(const QString &title, const SearchBarPosition searchBarPos = Bottom) : QMenu(title)
    {
        init(searchBarPos);
    }
    KateModeMenuList(QWidget *parent, const SearchBarPosition searchBarPos = Bottom) : QMenu(parent)
    {
        init(searchBarPos);
    }
    KateModeMenuList(const QString &title, QWidget *parent, const SearchBarPosition searchBarPos = Bottom) : QMenu(title, parent)
    {
        init(searchBarPos);
    }
    ~KateModeMenuList() { }

    /**
     * Update the selected item in the list widget, but without changing
     * the syntax highlighting in the document.
     * This is useful for updating this menu, when changing the syntax highlighting
     * from another menu, or from an external one. This doesn't hide or show the menu.
     * @param nameMode Raw name of the syntax highlight definition. If it's empty,
     *                 the "Normal" mode will be used.
     */
    void selectHighlightingFromExternal(const QString &nameMode);
    /**
     * Update the selected item in the list widget, but without changing
     * the syntax highlighting in the document. This doesn't hide or show the menu.
     * The menu is kept updated according to the active syntax highlighting,
     * obtained from the KTextEditor::DocumentPrivate class.
     * @see KTextEditor::DocumentPrivate::fileType()
     */
    void selectHighlightingFromExternal();

    /**
     * Set the button that shows this menu. It allows to update the label
     * of the button and define the alignment of the menu with respect to it.
     * @param button Trigger button.
     * @param bAutoUpdateTextButton Determines whether the text of the button should be
     *        changed when selecting an item from the menu.
     * @param position Position of the menu with respect to the trigger button.
     *        See KateModeMenuList::AlignmentButton.
     *
     * @see AlignmentButton
     */
    void setButton(QPushButton *button, const bool bAutoUpdateTextButton = false, AlignmentButton position = Inverse);

    /**
     * Define the size of the list widget, in pixels.
     */
    inline void setSizeList(const int height, const int width = 260);

    /**
     * Define the scroll when cleaning the search or changing the view.
     * The default value is AutoScroll::ScrollToSelectedItem.
     * @see AutoScroll
     */
    void setAutoScroll(AutoScroll scroll)
    {
        m_autoScroll = scroll;
    }

    /**
     * Set document to apply the syntax highlighting.
     * @see KTextEditor::DocumentPrivate
     */
    void updateMenu(KTextEditor::Document *doc);

protected:
    friend KateModeMenuListData::ListView;
    friend KateModeMenuListData::ListItem;
    friend KateModeMenuListData::SearchLine;

    /**
     * Action when displaying the menu.
     * Override from QWidget.
     */
    void showEvent(QShowEvent *event) override;

private:
    void init(const SearchBarPosition searchBarPos);

    /**
     * Load the data model with the syntax highlighting definitions to show in the list.
     */
    void loadHighlightingModel();

    /**
     * Scroll the list, according to AutoScroll.
     * @see AutoScroll
     */
    void autoScroll();

    /**
     * Set a custom word wrap on a text line, according to a maximum width (in pixels).
     * @param text Line of text
     * @param maxWidth Width of the text container, in pixels.
     * @param fontMetrics Font metrics. See QWidget::fontMetrics()
     */
    QString setWordWrap(const QString &text, const int maxWidth, const QFontMetrics &fontMetrics) const;

    /**
     * Update the selected item in the list, with the active syntax highlighting.
     * This method only changes the selected item, doesn't apply
     * syntax highlighting in the document, or hides the menu.
     * @see selectHighlighting(), selectHighlightingFromExternal(), selectHighlightingSetVisibility()
     */
    void updateSelectedItem(KateModeMenuListData::ListItem *item);

    /**
     * Select an item from the list and apply the syntax highlighting in the document.
     * This is equivalent to KateModeMenuList::selectHighlighting().
     * @param bHideMenu If the menu should be hidden after applying the highlight.
     * @see selectHighlighting()
     */
    void selectHighlightingSetVisibility(QStandardItem *pItem, const bool bHideMenu);

    /**
     * Load message when the list is empty in the search.
     */
    inline void loadEmptyMsg();

    AutoScroll m_autoScroll = ScrollToSelectedItem;
    AlignmentButton m_position;
    bool m_bAutoUpdateTextButton;

    QPushButton *m_pushButton = nullptr;
    QLabel *m_emptyListMsg = nullptr;
    QGridLayout *m_layoutList = nullptr;
    QScrollBar *m_scroll = nullptr;

    KateModeMenuListData::SearchLine *m_searchBar = nullptr;
    KateModeMenuListData::ListView *m_list = nullptr;
    QStandardItemModel *m_model = nullptr;

    /**
     * Item with active syntax highlighting.
     */
    KateModeMenuListData::ListItem *m_selectedItem = nullptr;

    /**
     * Icon for selected/active item (checkbox).
     * NOTE: Selected and inactive items show an icon with incorrect color,
     * however, this isn't a problem, since the list widget is never inactive.
     */
    const QIcon m_checkIcon = QIcon::fromTheme(QStringLiteral("checkbox"));
    static const int m_iconSize = 16;

    QPointer<KTextEditor::DocumentPrivate> m_doc;

private Q_SLOTS:
    /**
     * Action when selecting a item in the list. This also applies
     * the syntax highlighting in the document and hides the menu.
     * This is equivalent to KateModeMenuList::selectHighlightingSetVisibility().
     * @see selectHighlightingSetVisibility(), updateSelectedItem()
     */
    void selectHighlighting(const QModelIndex &index);
};


namespace KateModeMenuListData
{
    /**
     * Class of List Widget.
     */
    class ListView : public QListView
    {
        Q_OBJECT

    private:
        ListView(KateModeMenuList *menu) : QListView(menu)
        {
            m_parentMenu = menu;
        }

    public:
        /**
         * Define the size of the widget list.
         * @p height and @p width are values in pixels.
         */
        void setSizeList(const int height, const int width = 260);

        inline void setCurrentItem(const int rowItem)
        {
            selectionModel()->setCurrentIndex(m_parentMenu->m_model->index(rowItem, 0), QItemSelectionModel::ClearAndSelect);
        }
        inline QStandardItem* currentItem() const
        {
            return m_parentMenu->m_model->item(currentIndex().row(), 0);
        }

        inline void scrollToItem(const int rowItem, QAbstractItemView::ScrollHint hint = QAbstractItemView::PositionAtCenter)
        {
            scrollTo(m_parentMenu->m_model->index(rowItem, 0), hint);
        }

    protected:
        /**
         * Override from QListView.
         */
        void keyPressEvent(QKeyEvent *event) override;

    private:
        KateModeMenuList *m_parentMenu = nullptr;
        friend KateModeMenuList;
    };


    /**
     * Class of an Item of the Data Model of the List.
     * @see KateModeMenuListData::ListView, KateFileType, QStandardItemModel
     */
    class ListItem : public QStandardItem
    {
    private:
        ListItem() : QStandardItem() { }

        const KateFileType *m_type = nullptr;
        const QString *m_searchName = nullptr;

        friend KateModeMenuList;

    public:
        /**
         * Associate this item with a KateFileType object.
         */
        inline void setMode(KateFileType *type)
        {
            m_type = type;
        }
        const KateFileType* getMode() const
        {
            return m_type;
        }
        bool hasMode() const
        {
            return m_type;
        }

        /**
         * Generate name of the item used for the search.
         * @param itemName Pointer to the item name, can be an attribute of a KateFileType object.
         * @return True if a new name is generated for the search.
         */
        bool generateSearchName(const QString *itemName);

        /**
         * Find matches in the extensions of the item mode, with a @p text.
         * @param text Text to match, without dots or asterisks. For example, in
         *             a common extension, it corresponds to the text after "*."
         * @return True if a match is found, false if not.
         */
        bool matchExtension(const QString &text) const;

        const QString* getSearchName() const
        {
            return m_searchName;
        }
    };


    /**
     * Class of Search Bar.
     * Based on the KListWidgetSearchLine class.
     */
    class SearchLine : public QLineEdit
    {
        Q_OBJECT

    private:
        SearchLine(KateModeMenuList *menu) : QLineEdit(menu)
        {
            m_parentMenu = menu;
            init();
        }
        ~SearchLine() { };

        void init();

        /**
         * Select result of the items search.
         * Used only by KateModeMenuListData::SearchLine::updateSearch().
         */
        void setSearchResult(const int rowItem, bool &bEmptySection, int &lastSection, int &lastItem);

        /**
         * Delay in search results after typing, in milliseconds.
         * Default value: 200
         */
        static const int m_searchDelay = 170;

        bool m_bSearchStateAutoScroll = false;
        QString m_search = QString();
        int m_queuedSearches = 0;
        Qt::CaseSensitivity m_caseSensitivity = Qt::CaseInsensitive;

        KateModeMenuList *m_parentMenu = nullptr;
        friend KateModeMenuList;

    protected:
        /**
         * Override from QLineEdit. This allows you to navigate through
         * the menu and write in the search bar simultaneously with the keyboard.
         */
        void keyPressEvent(QKeyEvent *event) override;

    public Q_SLOTS:
        virtual void clear();
        virtual void updateSearch(const QString &s = QString());

    private Q_SLOTS:
        void _k_queueSearch(const QString &s);
        void _k_activateSearch();
    };
}

#endif // KATEMODEMENULIST_H