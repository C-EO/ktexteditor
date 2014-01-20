/* This file is part of the KDE libraries
   Copyright (C) 2009 Erlend Hamberg <ehamberg@gmail.com>
   Copyright (C) 2011 Svyatoslav Kuzmich <svatoslav1@gmail.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#ifndef VIMODE_APP_COMMANDS_INCLUDED
#define VIMODE_APP_COMMANDS_INCLUDED

#include <ktexteditor/commandinterface.h>
#include <QObject>

class KateViAppCommands : public QObject, public KTextEditor::Command
{
    Q_OBJECT

    KateViAppCommands();
    static KateViAppCommands* m_instance;

public:
    virtual ~KateViAppCommands();
    virtual const QStringList &cmds();
    virtual bool exec(KTextEditor::View *view, const QString &cmd, QString &msg);
    virtual bool help(KTextEditor::View *view, const QString &cmd, QString &msg);

    static KateViAppCommands* self() {
        if (m_instance == 0) {
            m_instance = new KateViAppCommands();
        }
        return m_instance;
    }
private Q_SLOTS:
    void closeCurrentDocument();
    void closeCurrentView();
    void quit();

private:
    QRegExp re_write;
    QRegExp re_close;
    QRegExp re_quit;
    QRegExp re_exit;
    QRegExp re_edit;
    QRegExp re_new;
    QRegExp re_split;
    QRegExp re_vsplit;
    QRegExp re_only;
};

#endif
