/*
    SPDX-FileCopyrightText: 2010 Christoph Cullmann <cullmann@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/
#include "config.h"

#include "katetextbuffer.h"
#include "katetextloader.h"

#include "katedocument.h"

// this is unfortunate, but needed for performance
#include "katepartdebug.h"
#include "kateview.h"

#ifndef Q_OS_WIN
#include <cerrno>
#include <unistd.h>
// sadly there seems to be no possibility in Qt to determine detailed error
// codes about e.g. file open errors, so we need to resort to evaluating
// errno directly on platforms that support this
#define CAN_USE_ERRNO
#endif

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QStringEncoder>
#include <QTemporaryFile>

#if HAVE_KAUTH
#include "katesecuretextbuffer_p.h"
#include <KAuth/Action>
#include <KAuth/ExecuteJob>
#endif

#if 0
#define BUFFER_DEBUG qCDebug(LOG_KTE)
#else
#define BUFFER_DEBUG                                                                                                                                           \
    if (0)                                                                                                                                                     \
    qCDebug(LOG_KTE)
#endif

namespace Kate
{
TextBuffer::TextBuffer(KTextEditor::DocumentPrivate *parent, bool alwaysUseKAuth)
    : QObject(parent)
    , m_document(parent)
    , m_history(*this)
    , m_lines(0)
    , m_revision(-1)
    , m_editingTransactions(0)
    , m_editingLastRevision(0)
    , m_editingLastLines(0)
    , m_editingMinimalLineChanged(-1)
    , m_editingMaximalLineChanged(-1)
    , m_encodingProberType(KEncodingProber::Universal)
    , m_generateByteOrderMark(false)
    , m_endOfLineMode(eolUnix)
    , m_lineLengthLimit(4096)
    , m_alwaysUseKAuthForSave(alwaysUseKAuth)
{
    // create initial state, this will set m_revision to 0
    clear();
}

TextBuffer::~TextBuffer()
{
    // remove document pointer, this will avoid any notifyAboutRangeChange to have a effect
    m_document = nullptr;

    // not allowed during editing
    Q_ASSERT(m_editingTransactions == 0);

    // invalidate all moving stuff
    std::vector<Kate::TextRange *> rangesWithFeedback;
    for (auto b : m_blocks) {
        auto cursors = std::move(b->m_cursors);
        for (auto it = cursors.begin(); it != cursors.end(); ++it) {
            auto cursor = *it;
            // update the block
            cursor->m_block = nullptr;
            cursor->m_line = cursor->m_column = -1;
            cursor->m_buffer = nullptr;
            if (auto r = cursor->kateRange()) {
                r->m_buffer = nullptr;
                if (r->feedback()) {
                    rangesWithFeedback.push_back(r);
                }
            }
        }
    }

    // uniquify ranges
    std::sort(rangesWithFeedback.begin(), rangesWithFeedback.end());
    auto it = std::unique(rangesWithFeedback.begin(), rangesWithFeedback.end());
    std::for_each(rangesWithFeedback.begin(), it, [](Kate::TextRange *range) {
        range->feedback()->rangeInvalid(range);
    });

    // clean out all cursors and lines, only cursors belonging to range will survive
    for (TextBlock *block : m_blocks) {
        block->clearLines();
    }

    // delete all blocks, now that all cursors are really deleted
    // else asserts in destructor of blocks will fail!
    qDeleteAll(m_blocks);
    m_blocks.clear();
}

void TextBuffer::invalidateRanges()
{
    std::vector<Kate::TextRange *> ranges;
    ranges.reserve(m_blocks.size());
    for (TextBlock *block : m_blocks) {
        for (auto cursor : block->m_cursors) {
            if (cursor->kateRange()) {
                ranges.push_back(cursor->kateRange());
            }
        }
    }
    // uniquify ranges
    std::sort(ranges.begin(), ranges.end());
    auto it = std::unique(ranges.begin(), ranges.end());
    std::for_each(ranges.begin(), it, [](Kate::TextRange *range) {
        range->setRange({KTextEditor::Cursor::invalid(), KTextEditor::Cursor::invalid()});
    });
}

void TextBuffer::clear()
{
    // not allowed during editing
    Q_ASSERT(m_editingTransactions == 0);

    m_multilineRanges.clear();
    invalidateRanges();

    // new block for empty buffer
    TextBlock *newBlock = new TextBlock(this, 0);
    newBlock->appendLine(QString());

    // clean out all cursors and lines, move them to newBlock if not belonging to a range
    for (TextBlock *block : std::as_const(m_blocks)) {
        auto cursors = std::move(block->m_cursors);
        for (auto it = cursors.begin(); it != cursors.end(); ++it) {
            auto cursor = *it;
            if (!cursor->kateRange()) {
                // update the block
                cursor->m_block = newBlock;
                // move the cursor into the target block
                cursor->m_line = cursor->m_column = 0;
                newBlock->m_cursors.push_back(cursor);
                // remove it and advance to next element
            }
            // skip cursors with ranges, we need to invalidate the ranges later
        }
        block->clearLines();
    }
    std::sort(newBlock->m_cursors.begin(), newBlock->m_cursors.end());

    // kill all buffer blocks
    qDeleteAll(m_blocks);
    // insert one block with one empty line
    m_blocks = {newBlock};
    m_startLines = {0};
    m_blockSizes = {1};

    // reset lines and last used block
    m_lines = 1;

    // increment revision, we did reset it here in the past
    // that is no good idea as we can mix up content variants after an reload
    ++m_revision;

    // reset bom detection
    m_generateByteOrderMark = false;

    // reset the filter device
    m_mimeTypeForFilterDev = QStringLiteral("text/plain");

    // clear edit history
    m_history.clear();

    // we got cleared
    Q_EMIT cleared();
}

TextLine TextBuffer::line(int line) const
{
    // get block, this will assert on invalid line
    int blockIndex = blockForLine(line);

    // get line
    return m_blocks.at(blockIndex)->line(line - m_startLines[blockIndex]);
}

void TextBuffer::setLineMetaData(int line, const TextLine &textLine)
{
    // get block, this will assert on invalid line
    int blockIndex = blockForLine(line);

    // get line
    return m_blocks.at(blockIndex)->setLineMetaData(line - m_startLines[blockIndex], textLine);
}

int TextBuffer::cursorToOffset(KTextEditor::Cursor c) const
{
    if ((c.line() < 0) || (c.line() >= lines())) {
        return -1;
    }

    int off = 0;
    const int blockIndex = blockForLine(c.line());
    for (auto it = m_blockSizes.begin(), end = m_blockSizes.begin() + blockIndex; it != end; ++it) {
        off += *it;
    }

    auto block = m_blocks[blockIndex];
    int start = block->startLine();
    int end = start + block->lines();
    for (int line = start; line < end; ++line) {
        if (line >= c.line()) {
            off += qMin(c.column(), block->lineLength(line));
            return off;
        }
        off += block->lineLength(line) + 1;
    }

    Q_ASSERT(false);
    return -1;
}

KTextEditor::Cursor TextBuffer::offsetToCursor(int offset) const
{
    if (offset >= 0) {
        int off = 0;
        int blockIdx = 0;
        for (int blockSize : m_blockSizes) {
            if (off + blockSize < offset) {
                off += blockSize;
            } else {
                auto block = m_blocks[blockIdx];
                const int lines = block->lines();
                int start = block->startLine();
                int end = start + lines;
                for (int line = start; line < end; ++line) {
                    const int len = block->lineLength(line);
                    if (off + len >= offset) {
                        return KTextEditor::Cursor(line, offset - off);
                    }
                    off += len + 1;
                }
            }
            blockIdx++;
        }
    }
    return KTextEditor::Cursor::invalid();
}

QString TextBuffer::text() const
{
    QString text;
    qsizetype size = 0;
    for (int blockSize : m_blockSizes) {
        size += blockSize;
    }
    text.reserve(size);
    size -= 1; // remove -1, last newline

    // combine all blocks
    for (TextBlock *block : m_blocks) {
        block->text(text);
    }
    text.chop(1); // remove last \n

    Q_ASSERT(size == text.size());
    return text;
}

bool TextBuffer::startEditing()
{
    // increment transaction counter
    ++m_editingTransactions;

    // if not first running transaction, do nothing
    if (m_editingTransactions > 1) {
        return false;
    }

    // reset information about edit...
    m_editingLastRevision = m_revision;
    m_editingLastLines = m_lines;
    m_editingMinimalLineChanged = -1;
    m_editingMaximalLineChanged = -1;

    // transaction has started
    Q_EMIT m_document->KTextEditor::Document::editingStarted(m_document);

    // first transaction started
    return true;
}

bool TextBuffer::finishEditing()
{
    // only allowed if still transactions running
    Q_ASSERT(m_editingTransactions > 0);

    // decrement counter
    --m_editingTransactions;

    // if not last running transaction, do nothing
    if (m_editingTransactions > 0) {
        return false;
    }

    // assert that if buffer changed, the line ranges are set and valid!
    Q_ASSERT(!editingChangedBuffer() || (m_editingMinimalLineChanged != -1 && m_editingMaximalLineChanged != -1));
    Q_ASSERT(!editingChangedBuffer() || (m_editingMinimalLineChanged <= m_editingMaximalLineChanged));
    Q_ASSERT(!editingChangedBuffer() || (m_editingMinimalLineChanged >= 0 && m_editingMinimalLineChanged < m_lines));
    Q_ASSERT(!editingChangedBuffer() || (m_editingMaximalLineChanged >= 0 && m_editingMaximalLineChanged < m_lines));

    // transaction has finished
    Q_EMIT m_document->KTextEditor::Document::editingFinished(m_document);

    // last transaction finished
    return true;
}

void TextBuffer::wrapLine(const KTextEditor::Cursor position)
{
    // debug output for REAL low-level debugging
    BUFFER_DEBUG << "wrapLine" << position;

    // only allowed if editing transaction running
    Q_ASSERT(m_editingTransactions > 0);

    // get block, this will assert on invalid line
    int blockIndex = blockForLine(position.line());

    // let the block handle the wrapLine
    // this can only lead to one more line in this block
    // no other blocks will change
    // this call will trigger fixStartLines
    ++m_lines; // first alter the line counter, as functions called will need the valid one
    m_blocks.at(blockIndex)->wrapLine(position, blockIndex);
    m_blockSizes[blockIndex] += 1;

    // remember changes
    ++m_revision;

    // update changed line interval
    if (position.line() < m_editingMinimalLineChanged || m_editingMinimalLineChanged == -1) {
        m_editingMinimalLineChanged = position.line();
    }

    if (position.line() <= m_editingMaximalLineChanged) {
        ++m_editingMaximalLineChanged;
    } else {
        m_editingMaximalLineChanged = position.line() + 1;
    }

    // balance the changed block if needed
    balanceBlock(blockIndex);

    // emit signal about done change
    Q_EMIT m_document->KTextEditor::Document::lineWrapped(m_document, position);
}

void TextBuffer::unwrapLine(int line)
{
    // debug output for REAL low-level debugging
    BUFFER_DEBUG << "unwrapLine" << line;

    // only allowed if editing transaction running
    Q_ASSERT(m_editingTransactions > 0);

    // line 0 can't be unwrapped
    Q_ASSERT(line > 0);

    // get block, this will assert on invalid line
    int blockIndex = blockForLine(line);

    // is this the first line in the block?
    const int blockStartLine = m_startLines[blockIndex];
    const bool firstLineInBlock = line == blockStartLine;

    // let the block handle the unwrapLine
    // this can either lead to one line less in this block or the previous one
    // the previous one could even end up with zero lines
    // this call will trigger fixStartLines

    m_blocks.at(blockIndex)
        ->unwrapLine(line - blockStartLine, (blockIndex > 0) ? m_blocks.at(blockIndex - 1) : nullptr, firstLineInBlock ? (blockIndex - 1) : blockIndex);
    --m_lines;

    // decrement index for later fixup, if we modified the block in front of the found one
    if (firstLineInBlock) {
        --blockIndex;
    }

    // remember changes
    ++m_revision;

    // update changed line interval
    if ((line - 1) < m_editingMinimalLineChanged || m_editingMinimalLineChanged == -1) {
        m_editingMinimalLineChanged = line - 1;
    }

    if (line <= m_editingMaximalLineChanged) {
        --m_editingMaximalLineChanged;
    } else {
        m_editingMaximalLineChanged = line - 1;
    }

    // balance the changed block if needed
    balanceBlock(blockIndex);

    // emit signal about done change
    Q_EMIT m_document->KTextEditor::Document::lineUnwrapped(m_document, line);
}

void TextBuffer::insertText(const KTextEditor::Cursor position, const QString &text)
{
    // debug output for REAL low-level debugging
    BUFFER_DEBUG << "insertText" << position << text;

    // only allowed if editing transaction running
    Q_ASSERT(m_editingTransactions > 0);

    // skip work, if no text to insert
    if (text.isEmpty()) {
        return;
    }

    // get block, this will assert on invalid line
    int blockIndex = blockForLine(position.line());

    // let the block handle the insertText
    m_blocks.at(blockIndex)->insertText(position, text);
    m_blockSizes[blockIndex] += text.size();

    // remember changes
    ++m_revision;

    // update changed line interval
    if (position.line() < m_editingMinimalLineChanged || m_editingMinimalLineChanged == -1) {
        m_editingMinimalLineChanged = position.line();
    }

    if (position.line() > m_editingMaximalLineChanged) {
        m_editingMaximalLineChanged = position.line();
    }

    // emit signal about done change
    Q_EMIT m_document->KTextEditor::Document::textInserted(m_document, position, text);
}

void TextBuffer::removeText(KTextEditor::Range range)
{
    // debug output for REAL low-level debugging
    BUFFER_DEBUG << "removeText" << range;

    // only allowed if editing transaction running
    Q_ASSERT(m_editingTransactions > 0);

    // only ranges on one line are supported
    Q_ASSERT(range.start().line() == range.end().line());

    // start column <= end column and >= 0
    Q_ASSERT(range.start().column() <= range.end().column());
    Q_ASSERT(range.start().column() >= 0);

    // skip work, if no text to remove
    if (range.isEmpty()) {
        return;
    }

    // get block, this will assert on invalid line
    int blockIndex = blockForLine(range.start().line());

    // let the block handle the removeText, retrieve removed text
    QString text;
    m_blocks.at(blockIndex)->removeText(range, text);
    m_blockSizes[blockIndex] -= text.size();

    // remember changes
    ++m_revision;

    // update changed line interval
    if (range.start().line() < m_editingMinimalLineChanged || m_editingMinimalLineChanged == -1) {
        m_editingMinimalLineChanged = range.start().line();
    }

    if (range.start().line() > m_editingMaximalLineChanged) {
        m_editingMaximalLineChanged = range.start().line();
    }

    // emit signal about done change
    Q_EMIT m_document->KTextEditor::Document::textRemoved(m_document, range, text);
}

int TextBuffer::blockForLine(int line) const
{
    // only allow valid lines
    if ((line < 0) || (line >= lines())) {
        qFatal("out of range line requested in text buffer (%d out of [0, %d])", line, lines());
    }

    size_t b = line / BufferBlockSize;
    if (b >= m_blocks.size()) {
        b = m_blocks.size() - 1;
    }

    if (m_startLines[b] <= line && line < m_startLines[b] + m_blocks[b]->lines()) {
        return b;
    }

    if (m_startLines[b] > line) {
        for (int i = b - 1; i >= 0; --i) {
            if (m_startLines[i] <= line && line < m_startLines[i] + m_blocks[i]->lines()) {
                return i;
            }
        }
    }

    if (m_startLines[b] < line || (m_blocks[b]->lines() == 0)) {
        for (size_t i = b + 1; i < m_blocks.size(); ++i) {
            if (m_startLines[i] <= line && line < m_startLines[i] + m_blocks[i]->lines()) {
                return i;
            }
        }
    }

    qFatal("line requested in text buffer (%d out of [0, %d[), no block found", line, lines());
    return -1;
}

void TextBuffer::fixStartLines(int startBlock, int value)
{
    // only allow valid start block
    Q_ASSERT(startBlock >= 0);
    Q_ASSERT(startBlock <= (int)m_startLines.size());
    // fixup block
    for (auto it = m_startLines.begin() + startBlock, end = m_startLines.end(); it != end; ++it) {
        // move start line by given value
        *it += value;
    }
}

void TextBuffer::balanceBlock(int index)
{
    auto check = qScopeGuard([this] {
        if (!(m_blocks.size() == m_startLines.size() && m_blocks.size() == m_blockSizes.size())) {
            qFatal("blocks/startlines/blocksizes are not equal in size!");
        }
    });

    // two cases, too big or too small block
    TextBlock *blockToBalance = m_blocks.at(index);

    // first case, too big one, split it
    if (blockToBalance->lines() >= 2 * BufferBlockSize) {
        // half the block
        int halfSize = blockToBalance->lines() / 2;

        // create and insert new block after current one, already set right start line
        const int newBlockStartLine = m_startLines[index] + halfSize;
        TextBlock *newBlock = new TextBlock(this, index + 1);
        m_blocks.insert(m_blocks.begin() + index + 1, newBlock);
        m_startLines.insert(m_startLines.begin() + index + 1, newBlockStartLine);
        m_blockSizes.insert(m_blockSizes.begin() + index + 1, 0);

        // adjust block indexes
        for (auto it = m_blocks.begin() + index, end = m_blocks.end(); it != end; ++it) {
            (*it)->setBlockIndex(index++);
        }

        blockToBalance->splitBlock(halfSize, newBlock);

        // split is done
        return;
    }

    // second case: possibly too small block

    // if only one block, no chance to unite
    // same if this is first block, we always append to previous one
    if (index == 0) {
        // remove the block if its empty
        if (blockToBalance->lines() == 0) {
            m_blocks.erase(m_blocks.begin());
            m_startLines.erase(m_startLines.begin());
            m_blockSizes.erase(m_blockSizes.begin());
            Q_ASSERT(m_startLines[0] == 0);
            for (auto it = m_blocks.begin(), end = m_blocks.end(); it != end; ++it) {
                (*it)->setBlockIndex(index++);
            }
        }
        return;
    }

    // block still large enough, do nothing
    if (2 * blockToBalance->lines() > BufferBlockSize) {
        return;
    }

    // unite small block with predecessor
    TextBlock *targetBlock = m_blocks.at(index - 1);

    // merge block
    blockToBalance->mergeBlock(targetBlock);
    m_blockSizes[index - 1] += m_blockSizes[index];

    // delete old block
    delete blockToBalance;
    m_blocks.erase(m_blocks.begin() + index);
    m_startLines.erase(m_startLines.begin() + index);
    m_blockSizes.erase(m_blockSizes.begin() + index);

    for (auto it = m_blocks.begin() + index, end = m_blocks.end(); it != end; ++it) {
        (*it)->setBlockIndex(index++);
    }

    Q_ASSERT(index == (int)m_blocks.size());
}

void TextBuffer::debugPrint(const QString &title) const
{
    // print header with title
    printf("%s (lines: %d)\n", qPrintable(title), m_lines);

    // print all blocks
    for (size_t i = 0; i < m_blocks.size(); ++i) {
        m_blocks.at(i)->debugPrint(i);
    }
}

bool TextBuffer::load(const QString &filename, bool &encodingErrors, bool &tooLongLinesWrapped, int &longestLineLoaded, bool enforceTextCodec)
{
    // fallback codec must exist
    Q_ASSERT(!m_fallbackTextCodec.isEmpty());

    // codec must be set!
    Q_ASSERT(!m_textCodec.isEmpty());

    // first: clear buffer in any case!
    clear();

    // construct the file loader for the given file, with correct prober type
    Kate::TextLoader file(filename, m_encodingProberType, m_lineLengthLimit);

    // triple play, maximal three loading rounds
    // 0) use the given encoding, be done, if no encoding errors happen
    // 1) use BOM to decided if Unicode or if that fails, use encoding prober, if no encoding errors happen, be done
    // 2) use fallback encoding, be done, if no encoding errors happen
    // 3) use again given encoding, be done in any case
    for (int i = 0; i < (enforceTextCodec ? 1 : 4); ++i) {
        // kill all blocks beside first one
        for (size_t b = 1; b < m_blocks.size(); ++b) {
            TextBlock *block = m_blocks.at(b);
            block->clearLines();
            delete block;
        }
        m_blocks.resize(1);
        m_startLines.resize(1);
        m_blockSizes.resize(1);

        // remove lines in first block
        m_blocks.back()->clearLines();
        m_startLines.back() = 0;
        m_blockSizes.back() = 0;
        m_lines = 0;

        // reset error flags
        tooLongLinesWrapped = false;
        longestLineLoaded = 0;

        // try to open file, with given encoding
        // in round 0 + 3 use the given encoding from user
        // in round 1 use 0, to trigger detection
        // in round 2 use fallback
        QString codec = m_textCodec;
        if (i == 1) {
            codec.clear();
        } else if (i == 2) {
            codec = m_fallbackTextCodec;
        }

        if (!file.open(codec)) {
            // create one dummy textline, in any case
            m_blocks.back()->appendLine(QString());
            m_lines++;
            m_blockSizes[0] = 1;
            return false;
        }

        // read in all lines...
        encodingErrors = false;
        while (!file.eof()) {
            // read line
            int offset = 0;
            int length = 0;
            bool currentError = !file.readLine(offset, length, tooLongLinesWrapped, longestLineLoaded);
            encodingErrors = encodingErrors || currentError;

            // bail out on encoding error, if not last round!
            if (encodingErrors && i < (enforceTextCodec ? 0 : 3)) {
                BUFFER_DEBUG << "Failed try to load file" << filename << "with codec" << file.textCodec();
                break;
            }

            // ensure blocks aren't too large
            if (m_blocks.back()->lines() >= BufferBlockSize) {
                int index = (int)m_blocks.size();
                int startLine = m_blocks.back()->startLine() + m_blocks.back()->lines();
                m_blocks.push_back(new TextBlock(this, index));
                m_startLines.push_back(startLine);
                m_blockSizes.push_back(0);
            }

            // append line to last block
            m_blocks.back()->appendLine(QString(file.unicode() + offset, length));
            m_blockSizes.back() += length + 1;
            ++m_lines;
        }

        // if no encoding error, break out of reading loop
        if (!encodingErrors) {
            // remember used codec, might change bom setting
            setTextCodec(file.textCodec());
            break;
        }
    }

    // save checksum of file on disk
    setDigest(file.digest());

    // remember if BOM was found
    if (file.byteOrderMarkFound()) {
        setGenerateByteOrderMark(true);
    }

    // remember eol mode, if any found in file
    if (file.eol() != eolUnknown) {
        setEndOfLineMode(file.eol());
    }

    // remember mime type for filter device
    m_mimeTypeForFilterDev = file.mimeTypeForFilterDev();

    // assert that one line is there!
    Q_ASSERT(m_lines > 0);

    // report CODEC + ERRORS
    BUFFER_DEBUG << "Loaded file " << filename << "with codec" << m_textCodec << (encodingErrors ? "with" : "without") << "encoding errors";

    // report BOM
    BUFFER_DEBUG << (file.byteOrderMarkFound() ? "Found" : "Didn't find") << "byte order mark";

    // report filter device mime-type
    BUFFER_DEBUG << "used filter device for mime-type" << m_mimeTypeForFilterDev;

    // emit success
    Q_EMIT loaded(filename, encodingErrors);

    // file loading worked, modulo encoding problems
    return true;
}

const QByteArray &TextBuffer::digest() const
{
    return m_digest;
}

void TextBuffer::setDigest(const QByteArray &checksum)
{
    m_digest = checksum;
}

void TextBuffer::setTextCodec(const QString &codec)
{
    m_textCodec = codec;

    // enforce bom for some encodings
    if (const auto setEncoding = QStringConverter::encodingForName(m_textCodec.toUtf8().constData())) {
        for (const auto encoding : {QStringConverter::Utf16,
                                    QStringConverter::Utf16BE,
                                    QStringConverter::Utf16LE,
                                    QStringConverter::Utf32,
                                    QStringConverter::Utf32BE,
                                    QStringConverter::Utf32LE}) {
            if (setEncoding == encoding) {
                setGenerateByteOrderMark(true);
                break;
            }
        }
    }
}

bool TextBuffer::save(const QString &filename)
{
    // codec must be set, else below we fail!
    Q_ASSERT(!m_textCodec.isEmpty());

    // ensure we do not kill symlinks, see bug 498589
    auto realFile = filename;
    if (const auto realFileResolved = QFileInfo(realFile).canonicalFilePath(); !realFileResolved.isEmpty()) {
        realFile = realFileResolved;
    }

    const auto saveRes = saveBufferUnprivileged(realFile);
    if (saveRes == SaveResult::Failed) {
        return false;
    }
    if (saveRes == SaveResult::MissingPermissions) {
        // either unit-test mode or we're missing permissions to write to the
        // file => use temporary file and try to use authhelper
        if (!saveBufferEscalated(realFile)) {
            return false;
        }
    }

    // remember this revision as last saved
    m_history.setLastSavedRevision();

    // inform that we have saved the state
    markModifiedLinesAsSaved();

    // emit that file was saved and be done
    Q_EMIT saved(filename);
    return true;
}

bool TextBuffer::saveBuffer(const QString &filename, KCompressionDevice &saveFile)
{
    QStringEncoder encoder(m_textCodec.toUtf8().constData(), generateByteOrderMark() ? QStringConverter::Flag::WriteBom : QStringConverter::Flag::Default);

    // our loved eol string ;)
    QString eol = QStringLiteral("\n");
    if (endOfLineMode() == eolDos) {
        eol = QStringLiteral("\r\n");
    } else if (endOfLineMode() == eolMac) {
        eol = QStringLiteral("\r");
    }

    // just dump the lines out ;)
    for (int i = 0; i < m_lines; ++i) {
        // dump current line
        saveFile.write(encoder.encode(line(i).text()));

        // append correct end of line string
        if ((i + 1) < m_lines) {
            saveFile.write(encoder.encode(eol));
        }

        // early out on stream errors
        if (saveFile.error() != QFileDevice::NoError) {
            return false;
        }
    }

    // TODO: this only writes bytes when there is text. This is a fine optimization for most cases, but this makes saving
    // an empty file with the BOM set impossible (results to an empty file with 0 bytes, no BOM)

    // close the file, we might want to read from underlying buffer below
    saveFile.close();

    // did save work?
    if (saveFile.error() != QFileDevice::NoError) {
        BUFFER_DEBUG << "Saving file " << filename << "failed with error" << saveFile.errorString();
        return false;
    }

    return true;
}

TextBuffer::SaveResult TextBuffer::saveBufferUnprivileged(const QString &filename)
{
    if (m_alwaysUseKAuthForSave) {
        // unit-testing mode, simulate we need privileges
        return SaveResult::MissingPermissions;
    }

    // construct correct filter device
    // we try to use the same compression as for opening
    const KCompressionDevice::CompressionType type = KCompressionDevice::compressionTypeForMimeType(m_mimeTypeForFilterDev);
    auto saveFile = std::make_unique<KCompressionDevice>(filename, type);

    if (!saveFile->open(QIODevice::WriteOnly)) {
#ifdef CAN_USE_ERRNO
        if (errno != EACCES) {
            return SaveResult::Failed;
        }
#endif
        return SaveResult::MissingPermissions;
    }

    if (!saveBuffer(filename, *saveFile)) {
        return SaveResult::Failed;
    }

    return SaveResult::Success;
}

bool TextBuffer::saveBufferEscalated(const QString &filename)
{
#if HAVE_KAUTH
    // construct correct filter device
    // we try to use the same compression as for opening
    const KCompressionDevice::CompressionType type = KCompressionDevice::compressionTypeForMimeType(m_mimeTypeForFilterDev);
    auto saveFile = std::make_unique<KCompressionDevice>(filename, type);
    uint ownerId = -2;
    uint groupId = -2;
    std::unique_ptr<QIODevice> temporaryBuffer;

    // Memorize owner and group.
    const QFileInfo fileInfo(filename);
    if (fileInfo.exists()) {
        ownerId = fileInfo.ownerId();
        groupId = fileInfo.groupId();
    }

    // if that fails we need more privileges to save this file
    // -> we write to a temporary file and then send its path to KAuth action for privileged save
    temporaryBuffer = std::make_unique<QBuffer>();

    // open buffer for write and read (read is used for checksum computing and writing to temporary file)
    if (!temporaryBuffer->open(QIODevice::ReadWrite)) {
        return false;
    }

    // we are now saving to a temporary buffer with potential compression proxy
    saveFile = std::make_unique<KCompressionDevice>(temporaryBuffer.get(), false, type);
    if (!saveFile->open(QIODevice::WriteOnly)) {
        return false;
    }

    if (!saveBuffer(filename, *saveFile)) {
        return false;
    }

    // temporary buffer was used to save the file
    // -> computing checksum
    // -> saving to temporary file
    // -> copying the temporary file to the original file location with KAuth action
    QTemporaryFile tempFile;
    if (!tempFile.open()) {
        return false;
    }

    // go to QBuffer start
    temporaryBuffer->seek(0);

    // read contents of QBuffer and add them to checksum utility as well as to QTemporaryFile
    char buffer[bufferLength];
    qint64 read = -1;
    QCryptographicHash cryptographicHash(SecureTextBuffer::checksumAlgorithm);
    while ((read = temporaryBuffer->read(buffer, bufferLength)) > 0) {
        cryptographicHash.addData(QByteArrayView(buffer, read));
        if (tempFile.write(buffer, read) == -1) {
            return false;
        }
    }
    if (!tempFile.flush()) {
        return false;
    }

    // prepare data for KAuth action
    QVariantMap kAuthActionArgs;
    kAuthActionArgs.insert(QStringLiteral("sourceFile"), tempFile.fileName());
    kAuthActionArgs.insert(QStringLiteral("targetFile"), filename);
    kAuthActionArgs.insert(QStringLiteral("checksum"), cryptographicHash.result());
    kAuthActionArgs.insert(QStringLiteral("ownerId"), ownerId);
    kAuthActionArgs.insert(QStringLiteral("groupId"), groupId);

    // call save with elevated privileges
    if (QStandardPaths::isTestModeEnabled()) {
        // unit testing purposes only
        if (!SecureTextBuffer::savefile(kAuthActionArgs).succeeded()) {
            return false;
        }
    } else {
        KAuth::Action kAuthSaveAction(QStringLiteral("org.kde.ktexteditor6.katetextbuffer.savefile"));
        kAuthSaveAction.setHelperId(QStringLiteral("org.kde.ktexteditor6.katetextbuffer"));
        kAuthSaveAction.setArguments(kAuthActionArgs);
        KAuth::ExecuteJob *job = kAuthSaveAction.execute();
        if (!job->exec()) {
            return false;
        }
    }

    return true;
#else
    Q_UNUSED(filename);
    return false;
#endif
}

void TextBuffer::notifyAboutRangeChange(KTextEditor::View *view, KTextEditor::LineRange lineRange, bool needsRepaint, TextRange *deleteRange)
{
    // ignore calls if no document is around
    if (!m_document) {
        return;
    }

    // update all views, this IS ugly and could be a signal, but I profiled and a signal is TOO slow, really
    // just create 20k ranges in a go and you wait seconds on a decent machine
    const QList<KTextEditor::View *> views = m_document->views();
    for (KTextEditor::View *curView : views) {
        // filter wrong views
        if (view && view != curView && !deleteRange) {
            continue;
        }

        // notify view, it is really a kate view
        static_cast<KTextEditor::ViewPrivate *>(curView)->notifyAboutRangeChange(lineRange, needsRepaint, deleteRange);
    }
}

void TextBuffer::markModifiedLinesAsSaved()
{
    for (TextBlock *block : std::as_const(m_blocks)) {
        block->markModifiedLinesAsSaved();
    }
}

void TextBuffer::addMultilineRange(TextRange *range)
{
    auto it = std::find(m_multilineRanges.begin(), m_multilineRanges.end(), range);
    if (it == m_multilineRanges.end()) {
        m_multilineRanges.push_back(range);
        return;
    }
}

void TextBuffer::removeMultilineRange(TextRange *range)
{
    m_multilineRanges.erase(std::remove(m_multilineRanges.begin(), m_multilineRanges.end(), range), m_multilineRanges.end());
}

bool TextBuffer::hasMultlineRange(KTextEditor::MovingRange *range) const
{
    return std::find(m_multilineRanges.begin(), m_multilineRanges.end(), range) != m_multilineRanges.end();
}

void TextBuffer::rangesForLine(int line, KTextEditor::View *view, bool rangesWithAttributeOnly, QList<TextRange *> &outRanges) const
{
    outRanges.clear();
    // get block, this will assert on invalid line
    const int blockIndex = blockForLine(line);
    m_blocks.at(blockIndex)->rangesForLine(line, view, rangesWithAttributeOnly, outRanges);
    // printf("Requested range for line %d, available %d\n", (int)line, (int)m_multilineRanges.size());
    for (TextRange *range : std::as_const(m_multilineRanges)) {
        if (rangesWithAttributeOnly && !range->hasAttribute()) {
            continue;
        }

        // we want ranges for no view, but this one's attribute is only valid for views
        if (!view && range->attributeOnlyForViews()) {
            continue;
        }

        // the range's attribute is not valid for this view
        if (range->view() && range->view() != view) {
            continue;
        }

        // if line is in the range, ok
        if (range->startInternal().lineInternal() <= line && line <= range->endInternal().lineInternal()) {
            outRanges.append(range);
        }
    }
    std::sort(outRanges.begin(), outRanges.end());
    outRanges.erase(std::unique(outRanges.begin(), outRanges.end()), outRanges.end());
}
}

#include "moc_katetextbuffer.cpp"
