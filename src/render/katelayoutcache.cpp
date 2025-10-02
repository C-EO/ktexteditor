/*
    SPDX-FileCopyrightText: 2005 Hamish Rodda <rodda@kde.org>
    SPDX-FileCopyrightText: 2008-2018 Dominik Haumann <dhaumann@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "katelayoutcache.h"

#include "katedocument.h"
#include "katepartdebug.h"
#include "katerenderer.h"
#include "kateview.h"

namespace
{
bool enableLayoutCache = false;

// for lower_bound
bool lessThan(KateLineLayout *lhs, int line)
{
    return lhs->line() < line;
}

// for upper_bound
bool lessThan2(int line, KateLineLayout *lhs)
{
    return line < lhs->line();
}
}

// BEGIN KateLineLayoutMap

KateLineLayoutMap::KateLineLayoutMap(std::pmr::unsynchronized_pool_resource &allocator)
    : m_allocator(allocator)
{
}

void KateLineLayoutMap::clear()
{
    for (auto l : m_lineLayouts) {
        l->~KateLineLayout();
        m_allocator.deallocate(l, sizeof(KateLineLayout), alignof(KateLineLayout));
    }

    const auto numLayouts = m_lineLayouts.size();
    m_lineLayouts.clear();
    // If we cleared a lot of layouts, release memory
    if (numLayouts > 1500) {
        m_allocator.release();
    }
}

void KateLineLayoutMap::insert(KateLineLayout *lineLayoutPtr)
{
    auto it = std::upper_bound(m_lineLayouts.begin(), m_lineLayouts.end(), lineLayoutPtr->line(), lessThan2);
    m_lineLayouts.insert(it, lineLayoutPtr);
}

void KateLineLayoutMap::relayoutLines(int startRealLine, int endRealLine)
{
    auto start = std::lower_bound(m_lineLayouts.begin(), m_lineLayouts.end(), startRealLine, lessThan);
    auto end = std::upper_bound(start, m_lineLayouts.end(), endRealLine, lessThan2);

    while (start != end) {
        (*start)->layoutDirty = true;
        ++start;
    }
}

void KateLineLayoutMap::slotEditDone(KateRenderer *renderer, int fromLine, int toLine, int shiftAmount, std::vector<KateTextLayout> &textLayouts)
{
    auto start = std::lower_bound(m_lineLayouts.begin(), m_lineLayouts.end(), fromLine, lessThan);
    auto end = std::upper_bound(start, m_lineLayouts.end(), toLine, lessThan2);

    if (shiftAmount != 0) {
        for (auto l : std::span(end, m_lineLayouts.end())) {
            l->setLine(renderer->folding(), l->line() + shiftAmount);
        }

        for (auto l : std::span(start, end)) {
            l->clear();
            for (auto &tl : textLayouts) {
                if (tl.kateLineLayout() == l) {
                    // Invalidate the layout, this will mark it as dirty
                    tl = KateTextLayout::invalid();
                }
            }
            l->~KateLineLayout();
            m_allocator.deallocate(l, sizeof(KateLineLayout), alignof(KateLineLayout));
        }
        m_lineLayouts.erase(start, end);
    } else {
        for (auto it = start; it != end; ++it) {
            (*it)->layoutDirty = true;
        }
    }
}

KateLineLayout *KateLineLayoutMap::find(int i)
{
    const auto it = std::lower_bound(m_lineLayouts.begin(), m_lineLayouts.end(), i, lessThan);
    if (it != m_lineLayouts.end() && (*it)->line() == i) {
        return *it;
    }
    return nullptr;
}
// END KateLineLayoutMap

KateLayoutCache::KateLayoutCache(KateRenderer *renderer, QObject *parent)
    : QObject(parent)
    , m_renderer(renderer)
    , m_allocator(std::pmr::pool_options{.largest_required_pool_block = sizeof(KateLineLayout)})
    , m_lineLayouts(m_allocator)
{
    Q_ASSERT(m_renderer);

    // connect to all possible editing primitives
    connect(m_renderer->doc(), &KTextEditor::Document::lineWrapped, this, &KateLayoutCache::wrapLine);
    connect(m_renderer->doc(), &KTextEditor::Document::lineUnwrapped, this, &KateLayoutCache::unwrapLine);
    connect(m_renderer->doc(), &KTextEditor::Document::textInserted, this, &KateLayoutCache::insertText);
    connect(m_renderer->doc(), &KTextEditor::Document::textRemoved, this, &KateLayoutCache::removeText);
}

void KateLayoutCache::updateViewCache(const KTextEditor::Cursor startPos, int newViewLineCount, int viewLinesScrolled)
{
    // qCDebug(LOG_KTE) << startPos << " nvlc " << newViewLineCount << " vls " << viewLinesScrolled;

    int oldViewLineCount = m_textLayouts.size();
    if (newViewLineCount == -1) {
        newViewLineCount = oldViewLineCount;
    }

    enableLayoutCache = true;

    int realLine;
    if (newViewLineCount == -1) {
        realLine = m_renderer->folding().visibleLineToLine(m_renderer->folding().lineToVisibleLine(startPos.line()));
    } else {
        realLine = m_renderer->folding().visibleLineToLine(startPos.line());
    }

    // compute the correct view line
    int _viewLine = 0;
    if (wrap()) {
        if (KateLineLayout *l = line(realLine)) {
            Q_ASSERT(l->isValid());
            Q_ASSERT(m_renderer->doc()->lineLength(l->line()) >= startPos.column() || m_renderer->view()->wrapCursor());
            bool found = false;
            for (; _viewLine < l->viewLineCount(); ++_viewLine) {
                const KateTextLayout &t = l->viewLine(_viewLine);
                if (t.startCol() >= startPos.column() || _viewLine == l->viewLineCount() - 1) {
                    found = true;
                    break;
                }
            }
            Q_ASSERT(found);
        }
    }

    m_startPos = startPos;

    // Move the text layouts if we've just scrolled...
    if (viewLinesScrolled != 0) {
        // loop backwards if we've just scrolled up...
        bool forwards = viewLinesScrolled >= 0 ? true : false;
        for (int z = forwards ? 0 : m_textLayouts.size() - 1; forwards ? ((size_t)z < m_textLayouts.size()) : (z >= 0); forwards ? z++ : z--) {
            int oldZ = z + viewLinesScrolled;
            if (oldZ >= 0 && (size_t)oldZ < m_textLayouts.size()) {
                m_textLayouts[z] = m_textLayouts[oldZ];
            }
        }
    }

    // Resize functionality
    if (newViewLineCount > oldViewLineCount) {
        m_textLayouts.reserve(newViewLineCount);
    } else if (newViewLineCount < oldViewLineCount) {
        m_textLayouts.resize(newViewLineCount);
    }

    KateLineLayout *l = line(realLine);
    for (int i = 0; i < newViewLineCount; ++i) {
        if (!l) {
            if ((size_t)i < m_textLayouts.size()) {
                if (m_textLayouts[i].isValid()) {
                    m_textLayouts[i] = KateTextLayout::invalid();
                }
            } else {
                m_textLayouts.push_back(KateTextLayout::invalid());
            }
            continue;
        }

        Q_ASSERT(l->isValid());
        Q_ASSERT(_viewLine < l->viewLineCount());

        if ((size_t)i < m_textLayouts.size()) {
            bool dirty = false;
            if (m_textLayouts[i].line() != realLine || m_textLayouts[i].viewLine() != _viewLine
                || (!m_textLayouts[i].isValid() && l->viewLine(_viewLine).isValid())) {
                dirty = true;
            }
            m_textLayouts[i] = l->viewLine(_viewLine);
            if (dirty) {
                m_textLayouts[i].setDirty(true);
            }

        } else {
            m_textLayouts.push_back(l->viewLine(_viewLine));
        }

        // qCDebug(LOG_KTE) << "Laid out line " << realLine << " (" << l << "), viewLine " << _viewLine << " (" << m_textLayouts[i].kateLineLayout().data() <<
        // ")"; m_textLayouts[i].debugOutput();

        _viewLine++;

        if (_viewLine > l->viewLineCount() - 1) {
            int virtualLine = l->virtualLine() + 1;
            realLine = m_renderer->folding().visibleLineToLine(virtualLine);
            _viewLine = 0;
            if (realLine < m_renderer->doc()->lines()) {
                l = line(realLine, virtualLine);
            } else {
                l = nullptr;
            }
        }
    }

    enableLayoutCache = false;
}

KateLineLayout *KateLayoutCache::line(int realLine, int virtualLine)
{
    if (auto l = m_lineLayouts.find(realLine)) {
        // ensure line is OK
        Q_ASSERT(l->line() == realLine);
        Q_ASSERT(realLine < m_renderer->doc()->lines());

        if (virtualLine != -1) {
            l->setVirtualLine(virtualLine);
        }

        const Kate::TextLine textLine = acceptDirtyLayouts() ? m_renderer->doc()->plainKateTextLine(l->line()) : m_renderer->doc()->kateTextLine(l->line());

        if (l->layout().lineCount() <= 0) {
            m_renderer->layoutLine(textLine, l, wrap() ? m_viewWidth : -1, enableLayoutCache);
        } else if (l->layoutDirty && !acceptDirtyLayouts()) {
            m_renderer->layoutLine(textLine, l, wrap() ? m_viewWidth : -1, enableLayoutCache);
        }

        Q_ASSERT(l->layout().lineCount() > 0 && (!l->layoutDirty || acceptDirtyLayouts()));

        return l;
    }

    if (realLine < 0 || realLine >= m_renderer->doc()->lines()) {
        return nullptr;
    }

    void *memory = m_allocator.allocate(sizeof(KateLineLayout), alignof(KateLineLayout));
    auto *l = new (memory) KateLineLayout;

    l->setLine(m_renderer->folding(), realLine, virtualLine);

    // because it may not have the syntax highlighting applied, allow layoutLine to use plainLines...
    const Kate::TextLine textLine = acceptDirtyLayouts() ? m_renderer->doc()->plainKateTextLine(l->line()) : m_renderer->doc()->kateTextLine(l->line());
    m_renderer->layoutLine(textLine, l, wrap() ? m_viewWidth : -1, enableLayoutCache);
    Q_ASSERT(l->isValid());

    if (acceptDirtyLayouts()) {
        l->layoutDirty = true;
    }

    // transfer ownership to m_lineLayouts
    m_lineLayouts.insert(l);
    return l;
}

KateTextLayout KateLayoutCache::textLayout(const KTextEditor::Cursor realCursor)
{
    return textLayout(realCursor.line(), viewLine(realCursor));
}

KateTextLayout KateLayoutCache::textLayout(uint realLine, int _viewLine)
{
    auto l = line(realLine);
    if (l && l->isValid()) {
        return l->viewLine(_viewLine);
    }
    return KateTextLayout::invalid();
}

KateTextLayout &KateLayoutCache::viewLine(int _viewLine)
{
    Q_ASSERT(_viewLine >= 0 && (size_t)_viewLine < m_textLayouts.size());
    return m_textLayouts[_viewLine];
}

int KateLayoutCache::viewCacheLineCount() const
{
    return m_textLayouts.size();
}

KTextEditor::Cursor KateLayoutCache::viewCacheStart() const
{
    return !m_textLayouts.empty() ? m_textLayouts.front().start() : KTextEditor::Cursor();
}

int KateLayoutCache::viewWidth() const
{
    return m_viewWidth;
}

/**
 * This returns the view line upon which realCursor is situated.
 * The view line is the number of lines in the view from the first line
 * The supplied cursor should be in real lines.
 */
int KateLayoutCache::viewLine(const KTextEditor::Cursor realCursor)
{
    /**
     * Make sure cursor column and line is valid
     */
    if (realCursor.column() < 0 || realCursor.line() < 0 || realCursor.line() > m_renderer->doc()->lines()) {
        return 0;
    }

    KateLineLayout *thisLine = line(realCursor.line());
    if (!thisLine) {
        return 0;
    }

    for (int i = 0; i < thisLine->viewLineCount(); ++i) {
        const KateTextLayout &l = thisLine->viewLine(i);
        if (realCursor.column() >= l.startCol() && realCursor.column() < l.endCol()) {
            return i;
        }
    }

    return thisLine->viewLineCount() - 1;
}

int KateLayoutCache::displayViewLine(const KTextEditor::Cursor virtualCursor, bool limitToVisible)
{
    if (!virtualCursor.isValid()) {
        return -1;
    }

    KTextEditor::Cursor work = viewCacheStart();

    // only try this with valid lines!
    if (work.isValid()) {
        work.setLine(m_renderer->folding().lineToVisibleLine(work.line()));
    }

    if (!work.isValid()) {
        return virtualCursor.line();
    }

    int limit = m_textLayouts.size();

    // Efficient non-word-wrapped path
    if (!m_renderer->view()->dynWordWrap()) {
        int ret = virtualCursor.line() - work.line();
        if (limitToVisible && (ret < 0)) {
            return -1;
        } else if (limitToVisible && (ret > limit)) {
            return -2;
        } else {
            return ret;
        }
    }

    if (work == virtualCursor) {
        return 0;
    }

    int ret = -(int)viewLine(viewCacheStart());
    bool forwards = (work < virtualCursor);

    // FIXME switch to using ranges? faster?
    if (forwards) {
        while (work.line() != virtualCursor.line()) {
            ret += viewLineCount(m_renderer->folding().visibleLineToLine(work.line()));
            work.setLine(work.line() + 1);
            if (limitToVisible && ret > limit) {
                return -2;
            }
        }
    } else {
        while (work.line() != virtualCursor.line()) {
            work.setLine(work.line() - 1);
            ret -= viewLineCount(m_renderer->folding().visibleLineToLine(work.line()));
            if (limitToVisible && ret < 0) {
                return -1;
            }
        }
    }

    // final difference
    KTextEditor::Cursor realCursor = virtualCursor;
    realCursor.setLine(m_renderer->folding().visibleLineToLine(realCursor.line()));
    if (realCursor.column() == -1) {
        realCursor.setColumn(m_renderer->doc()->lineLength(realCursor.line()));
    }
    ret += viewLine(realCursor);

    if (limitToVisible && ret < 0) {
        return -1;
    } else if (limitToVisible && ret > limit) {
        return -2;
    }

    return ret;
}

int KateLayoutCache::lastViewLine(int realLine)
{
    if (!m_renderer->view()->dynWordWrap()) {
        return 0;
    }

    if (KateLineLayout *l = line(realLine)) {
        return l->viewLineCount() - 1;
    }
    return 0;
}

int KateLayoutCache::viewLineCount(int realLine)
{
    return lastViewLine(realLine) + 1;
}

void KateLayoutCache::viewCacheDebugOutput() const
{
    qCDebug(LOG_KTE) << "Printing values for " << m_textLayouts.size() << " lines:";
    for (const KateTextLayout &t : std::as_const(m_textLayouts)) {
        if (t.isValid()) {
            t.debugOutput();
        } else {
            qCDebug(LOG_KTE) << "Line Invalid.";
        }
    }
}

void KateLayoutCache::wrapLine(KTextEditor::Document *, const KTextEditor::Cursor position)
{
    m_lineLayouts.slotEditDone(m_renderer, position.line(), position.line() + 1, 1, m_textLayouts);
}

void KateLayoutCache::unwrapLine(KTextEditor::Document *, int line)
{
    m_lineLayouts.slotEditDone(m_renderer, line - 1, line, -1, m_textLayouts);
}

void KateLayoutCache::insertText(KTextEditor::Document *, const KTextEditor::Cursor position, const QString &)
{
    m_lineLayouts.slotEditDone(m_renderer, position.line(), position.line(), 0, m_textLayouts);
}

void KateLayoutCache::removeText(KTextEditor::Document *, KTextEditor::Range range, const QString &)
{
    m_lineLayouts.slotEditDone(m_renderer, range.start().line(), range.start().line(), 0, m_textLayouts);
}

void KateLayoutCache::clear()
{
    m_textLayouts.clear();
    m_lineLayouts.clear();
    m_startPos = KTextEditor::Cursor(-1, -1);
}

void KateLayoutCache::setViewWidth(int width)
{
    m_viewWidth = width;
    clear();
}

bool KateLayoutCache::wrap() const
{
    return m_wrap;
}

void KateLayoutCache::setWrap(bool wrap)
{
    m_wrap = wrap;
    clear();
}

void KateLayoutCache::relayoutLines(int startRealLine, int endRealLine)
{
    if (startRealLine > endRealLine) {
        qCWarning(LOG_KTE) << "start" << startRealLine << "before end" << endRealLine;
    }

    m_lineLayouts.relayoutLines(startRealLine, endRealLine);
}

bool KateLayoutCache::acceptDirtyLayouts() const
{
    return m_acceptDirtyLayouts;
}

void KateLayoutCache::setAcceptDirtyLayouts(bool accept)
{
    m_acceptDirtyLayouts = accept;
}
