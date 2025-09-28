#include <algorithm>
#include <string>
#include <set>
#include <cstring>
#include <cctype>
#include <cfloat>
#include <regex>
#include <boost/regex.hpp>

#include "TextEditor.h"

#define IMGUI_SCROLLBAR_WIDTH 14.0f
#define POS_TO_COORDS_COLUMN_OFFSET 0.33f
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h" // for imGui::GetCurrentWindow()

namespace
{
	constexpr float FIND_REFRESH_DEFER_SECONDS = 0.12f;
}


struct TextEditor::RegexList {
    std::vector<std::pair<boost::regex, TextEditor::PaletteIndex>> mValue;
};


// --------------------------------------- //
// ------------- Exposed API ------------- //

TextEditor::TextEditor()
    : mRegexList(std::make_shared<RegexList>())
{
	SetPalette(defaultPalette);
	mLines.push_back(Line());
	std::memset(mFindBuffer, 0, sizeof(mFindBuffer));
	std::memset(mReplaceBuffer, 0, sizeof(mReplaceBuffer));
	mFindRefreshPending = false;
	mFindRefreshTimer = 0.0f;
}

TextEditor::~TextEditor()
{
}

void TextEditor::SetPalette(PaletteId aValue)
{
	mPaletteId = aValue;
	const Palette* palletteBase;
	switch (mPaletteId)
	{
	case PaletteId::Dark:
		palletteBase = &(GetDarkPalette());
		break;
	case PaletteId::Light:
		palletteBase = &(GetLightPalette());
		break;
	case PaletteId::Mariana:
		palletteBase = &(GetMarianaPalette());
		break;
	case PaletteId::RetroBlue:
		palletteBase = &(GetRetroBluePalette());
		break;
	}
	/* Update palette with the current alpha from style */
	for (int i = 0; i < (int)PaletteIndex::Max; ++i)
	{
		ImVec4 color = U32ColorToVec4((*palletteBase)[i]);
		color.w *= ImGui::GetStyle().Alpha;
		mPalette[i] = ImGui::ColorConvertFloat4ToU32(color);
	}
}

void TextEditor::SetLanguageDefinition(LanguageDefinitionId aValue)
{
	mLanguageDefinitionId = aValue;
	switch (mLanguageDefinitionId)
	{
	case LanguageDefinitionId::None:
		mLanguageDefinition = nullptr;
		return;
	case LanguageDefinitionId::Cpp:
		mLanguageDefinition = &(LanguageDefinition::Cpp());
		break;
	case LanguageDefinitionId::C:
		mLanguageDefinition = &(LanguageDefinition::C());
		break;
	case LanguageDefinitionId::Cs:
		mLanguageDefinition = &(LanguageDefinition::Cs());
		break;
	case LanguageDefinitionId::Python:
		mLanguageDefinition = &(LanguageDefinition::Python());
		break;
	case LanguageDefinitionId::Lua:
		mLanguageDefinition = &(LanguageDefinition::Lua());
		break;
	case LanguageDefinitionId::Json:
		mLanguageDefinition = &(LanguageDefinition::Json());
		break;
	case LanguageDefinitionId::Sql:
		mLanguageDefinition = &(LanguageDefinition::Sql());
		break;
	case LanguageDefinitionId::AngelScript:
		mLanguageDefinition = &(LanguageDefinition::AngelScript());
		break;
	case LanguageDefinitionId::Glsl:
		mLanguageDefinition = &(LanguageDefinition::Glsl());
		break;
	case LanguageDefinitionId::Hlsl:
		mLanguageDefinition = &(LanguageDefinition::Hlsl());
		break;
	}

    mRegexList->mValue.clear();
	for (const auto& r : mLanguageDefinition->mTokenRegexStrings)
        mRegexList->mValue.push_back(std::make_pair(boost::regex(r.first, boost::regex_constants::optimize), r.second));

	Colorize();
}

const char* TextEditor::GetLanguageDefinitionName() const
{
	return mLanguageDefinition != nullptr ? mLanguageDefinition->mName.c_str() : "None";
}

void TextEditor::SetTabSize(int aValue)
{
	mTabSize = Max(1, Min(8, aValue));
}

void TextEditor::SetLineSpacing(float aValue)
{
	mLineSpacing = Max(1.0f, Min(2.0f, aValue));
}

void TextEditor::SelectAll()
{
	ClearSelections();
	ClearExtraCursors();
	MoveTop();
	MoveBottom(true);
}

void TextEditor::SelectLine(int aLine)
{
	ClearSelections();
	ClearExtraCursors();
	SetSelection({ aLine, 0 }, { aLine, GetLineMaxColumn(aLine) });
}

void TextEditor::SelectRegion(int aStartLine, int aStartChar, int aEndLine, int aEndChar)
{
	ClearSelections();
	ClearExtraCursors();
	SetSelection(aStartLine, aStartChar, aEndLine, aEndChar);
}

void TextEditor::SelectNextOccurrenceOf(const char* aText, int aTextSize, bool aCaseSensitive)
{
	ClearSelections();
	ClearExtraCursors();
	SelectNextOccurrenceOf(aText, aTextSize, -1, aCaseSensitive);
}

void TextEditor::SelectAllOccurrencesOf(const char* aText, int aTextSize, bool aCaseSensitive)
{
	ClearSelections();
	ClearExtraCursors();
	SelectNextOccurrenceOf(aText, aTextSize, -1, aCaseSensitive);
	Coordinates startPos = mState.mCursors[mState.GetLastAddedCursorIndex()].mInteractiveEnd;
	while (true)
	{
		AddCursorForNextOccurrence(aCaseSensitive);
		Coordinates lastAddedPos = mState.mCursors[mState.GetLastAddedCursorIndex()].mInteractiveEnd;
		if (lastAddedPos == startPos)
			break;
	}
}

bool TextEditor::AnyCursorHasSelection() const
{
	for (int c = 0; c <= mState.mCurrentCursor; c++)
		if (mState.mCursors[c].HasSelection())
			return true;
	return false;
}

bool TextEditor::AllCursorsHaveSelection() const
{
	for (int c = 0; c <= mState.mCurrentCursor; c++)
		if (!mState.mCursors[c].HasSelection())
			return false;
	return true;
}

bool TextEditor::TryGetSelectionBounds(Coordinates& outStart, Coordinates& outEnd) const
{
	bool hasBounds = false;
	for (int c = 0; c <= mState.mCurrentCursor; ++c)
	{
		if (!mState.mCursors[c].HasSelection())
			continue;

		Coordinates selectionStart = mState.mCursors[c].GetSelectionStart();
		Coordinates selectionEnd = mState.mCursors[c].GetSelectionEnd();
		if (selectionEnd < selectionStart)
			std::swap(selectionStart, selectionEnd);

		selectionStart = SanitizeCoordinates(selectionStart);
		selectionEnd = SanitizeCoordinates(selectionEnd);

		if (!hasBounds || selectionStart < outStart)
			outStart = selectionStart;
		if (!hasBounds || outEnd < selectionEnd)
			outEnd = selectionEnd;
		hasBounds = true;
	}

	if (!hasBounds && mFindSelectionRangeValid)
	{
		Coordinates start = SanitizeCoordinates(mFindSelectionRangeStart);
		Coordinates end = SanitizeCoordinates(mFindSelectionRangeEnd);
		if (start < end)
		{
			outStart = start;
			outEnd = end;
			return true;
		}
	}

	if (!hasBounds || !(outStart < outEnd))
		return false;

	return true;
}

void TextEditor::MarkFindResultsDirty(bool deferRefresh)
{
	mFindResultsDirty = true;
	if (deferRefresh)
	{
		mFindRefreshPending = true;
		mFindRefreshTimer = FIND_REFRESH_DEFER_SECONDS;
	}
	else
	{
		mFindRefreshPending = false;
		mFindRefreshTimer = 0.0f;
	}
}

void TextEditor::ClearExtraCursors()
{
	mState.mCurrentCursor = 0;
}

void TextEditor::ClearSelections()
{
	for (int c = mState.mCurrentCursor; c > -1; c--)
		mState.mCursors[c].mInteractiveEnd =
		mState.mCursors[c].mInteractiveStart =
		mState.mCursors[c].GetSelectionEnd();
}

void TextEditor::SetCursorPosition(int aLine, int aCharIndex)
{
	SetCursorPosition({ aLine, GetCharacterColumn(aLine, aCharIndex) }, -1, true);
}

int TextEditor::GetFirstVisibleLine()
{
	return mFirstVisibleLine;
}

int TextEditor::GetLastVisibleLine()
{
	return mLastVisibleLine;
}

void TextEditor::SetViewAtLine(int aLine, SetViewAtLineMode aMode)
{
	mSetViewAtLine = aLine;
	mSetViewAtLineMode = aMode;
}

void TextEditor::Copy()
{
	if (AnyCursorHasSelection())
	{
		std::string clipboardText = GetClipboardText();
		ImGui::SetClipboardText(clipboardText.c_str());
	}
	else
	{
		if (!mLines.empty())
		{
			std::string str;
			auto& line = mLines[GetSanitizedCursorCoordinates().mLine];
			for (auto& g : line)
				str.push_back(g.mChar);
			ImGui::SetClipboardText(str.c_str());
		}
	}
}

void TextEditor::Cut()
{
	if (mReadOnly)
	{
		Copy();
	}
	else
	{
		if (AnyCursorHasSelection())
		{
			UndoRecord u;
			u.mBefore = mState;

			Copy();
			for (int c = mState.mCurrentCursor; c > -1; c--)
			{
				u.mOperations.push_back({ GetSelectedText(c), mState.mCursors[c].GetSelectionStart(), mState.mCursors[c].GetSelectionEnd(), UndoOperationType::Delete });
				DeleteSelection(c);
			}

			u.mAfter = mState;
			AddUndo(u);
		}
	}
}

void TextEditor::Paste()
{
	if (mReadOnly)
		return;

	if (ImGui::GetClipboardText() == nullptr)
		return; // something other than text in the clipboard

	// check if we should do multicursor paste
	std::string clipText = ImGui::GetClipboardText();
	bool canPasteToMultipleCursors = false;
	std::vector<std::pair<int, int>> clipTextLines;
	if (mState.mCurrentCursor > 0)
	{
		clipTextLines.push_back({ 0,0 });
		for (int i = 0; i < clipText.length(); i++)
		{
			if (clipText[i] == '\n')
			{
				clipTextLines.back().second = i;
				clipTextLines.push_back({ i + 1, 0 });
			}
		}
		clipTextLines.back().second = clipText.length();
		canPasteToMultipleCursors = clipTextLines.size() == mState.mCurrentCursor + 1;
	}

	if (clipText.length() > 0)
	{
		UndoRecord u;
		u.mBefore = mState;

		if (AnyCursorHasSelection())
		{
			for (int c = mState.mCurrentCursor; c > -1; c--)
			{
				u.mOperations.push_back({ GetSelectedText(c), mState.mCursors[c].GetSelectionStart(), mState.mCursors[c].GetSelectionEnd(), UndoOperationType::Delete });
				DeleteSelection(c);
			}
		}

		for (int c = mState.mCurrentCursor; c > -1; c--)
		{
			Coordinates start = GetSanitizedCursorCoordinates(c);
			if (canPasteToMultipleCursors)
			{
				std::string clipSubText = clipText.substr(clipTextLines[c].first, clipTextLines[c].second - clipTextLines[c].first);
				InsertTextAtCursor(clipSubText.c_str(), c);
				u.mOperations.push_back({ clipSubText, start, GetSanitizedCursorCoordinates(c), UndoOperationType::Add });
			}
			else
			{
				InsertTextAtCursor(clipText.c_str(), c);
				u.mOperations.push_back({ clipText, start, GetSanitizedCursorCoordinates(c), UndoOperationType::Add });
			}
		}

		u.mAfter = mState;
		AddUndo(u);
	}
}

void TextEditor::Undo(int aSteps)
{
	while (CanUndo() && aSteps-- > 0)
		mUndoBuffer[--mUndoIndex].Undo(this);
}

void TextEditor::Redo(int aSteps)
{
	while (CanRedo() && aSteps-- > 0)
		mUndoBuffer[mUndoIndex++].Redo(this);
}

void TextEditor::SetText(const std::string& aText)
{
	mLines.clear();
	mLines.emplace_back(Line());
	for (auto chr : aText)
	{
		if (chr == '\r')
			continue;

		if (chr == '\n')
			mLines.emplace_back(Line());
		else
		{
			mLines.back().emplace_back(Glyph(chr, PaletteIndex::Default));
		}
	}

	mScrollToTop = true;

	mUndoBuffer.clear();
	mUndoIndex = 0;

	Colorize();
	MarkFindResultsDirty(false);
	mFindResultIndex = -1;
	mFindHighlightsCache.clear();
}

std::string TextEditor::GetText() const
{
	auto lastLine = (int)mLines.size() - 1;
	auto lastLineLength = GetLineMaxColumn(lastLine);
	Coordinates startCoords = Coordinates();
	Coordinates endCoords = Coordinates(lastLine, lastLineLength);
	return startCoords < endCoords ? GetText(startCoords, endCoords) : "";
}

void TextEditor::SetTextLines(const std::vector<std::string>& aLines)
{
	mLines.clear();

	if (aLines.empty())
		mLines.emplace_back(Line());
	else
	{
		mLines.resize(aLines.size());

		for (size_t i = 0; i < aLines.size(); ++i)
		{
			const std::string& aLine = aLines[i];

			mLines[i].reserve(aLine.size());
			for (size_t j = 0; j < aLine.size(); ++j)
				mLines[i].emplace_back(Glyph(aLine[j], PaletteIndex::Default));
		}
	}

	mScrollToTop = true;

	mUndoBuffer.clear();
	mUndoIndex = 0;

	Colorize();
	MarkFindResultsDirty(false);
	mFindResultIndex = -1;
	mFindHighlightsCache.clear();
}

std::vector<std::string> TextEditor::GetTextLines() const
{
	std::vector<std::string> result;

	result.reserve(mLines.size());

	for (auto& line : mLines)
	{
		std::string text;

		text.resize(line.size());

		for (size_t i = 0; i < line.size(); ++i)
			text[i] = line[i].mChar;

		result.emplace_back(std::move(text));
	}

	return result;
}

bool TextEditor::Render(const char* aTitle, bool aParentIsFocused, const ImVec2& aSize, bool aBorder)
{
	if (mCursorPositionChanged)
		OnCursorPositionChanged();
	mCursorPositionChanged = false;

	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(mPalette[(int)PaletteIndex::Background]));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

	ImGui::BeginChild(aTitle, aSize, aBorder, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs);

	bool isFocused = ImGui::IsWindowFocused();
	HandleKeyboardInputs(aParentIsFocused);
	HandleMouseInputs();
	ColorizeInternal();
	Render(aParentIsFocused);

	ImGui::EndChild();

	ImVec2 panelMin = ImGui::GetItemRectMin();
	ImVec2 panelMax = ImGui::GetItemRectMax();
	RenderFindReplacePanel(panelMin, ImVec2(panelMax.x - panelMin.x, panelMax.y - panelMin.y), isFocused || aParentIsFocused);
	
	// Render auto-complete popup for SQL
	if (mLanguageDefinitionId == LanguageDefinitionId::Sql)
		RenderAutoComplete();

	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	return isFocused;
}

// ------------------------------------ //
// ---------- Generic utils ----------- //

// https://en.wikipedia.org/wiki/UTF-8
// We assume that the char is a standalone character (<128) or a leading byte of an UTF-8 code sequence (non-10xxxxxx code)
static int UTF8CharLength(char c)
{
	if ((c & 0xFE) == 0xFC)
		return 6;
	if ((c & 0xFC) == 0xF8)
		return 5;
	if ((c & 0xF8) == 0xF0)
		return 4;
	else if ((c & 0xF0) == 0xE0)
		return 3;
	else if ((c & 0xE0) == 0xC0)
		return 2;
	return 1;
}

// "Borrowed" from ImGui source
static inline int ImTextCharToUtf8(char* buf, int buf_size, unsigned int c)
{
	if (c < 0x80)
	{
		buf[0] = (char)c;
		return 1;
	}
	if (c < 0x800)
	{
		if (buf_size < 2) return 0;
		buf[0] = (char)(0xc0 + (c >> 6));
		buf[1] = (char)(0x80 + (c & 0x3f));
		return 2;
	}
	if (c >= 0xdc00 && c < 0xe000)
	{
		return 0;
	}
	if (c >= 0xd800 && c < 0xdc00)
	{
		if (buf_size < 4) return 0;
		buf[0] = (char)(0xf0 + (c >> 18));
		buf[1] = (char)(0x80 + ((c >> 12) & 0x3f));
		buf[2] = (char)(0x80 + ((c >> 6) & 0x3f));
		buf[3] = (char)(0x80 + ((c) & 0x3f));
		return 4;
	}
	//else if (c < 0x10000)
	{
		if (buf_size < 3) return 0;
		buf[0] = (char)(0xe0 + (c >> 12));
		buf[1] = (char)(0x80 + ((c >> 6) & 0x3f));
		buf[2] = (char)(0x80 + ((c) & 0x3f));
		return 3;
	}
}

static inline bool CharIsWordChar(char ch)
{
	int sizeInBytes = UTF8CharLength(ch);
	return sizeInBytes > 1 ||
		ch >= 'a' && ch <= 'z' ||
		ch >= 'A' && ch <= 'Z' ||
		ch >= '0' && ch <= '9' ||
		ch == '_';
}

// ------------------------------------ //
// ------------- Internal ------------- //


// ---------- Editor state functions --------- //

void TextEditor::EditorState::AddCursor()
{
	// vector is never resized to smaller size, mCurrentCursor points to last available cursor in vector
	mCurrentCursor++;
	mCursors.resize(mCurrentCursor + 1);
	mLastAddedCursor = mCurrentCursor;
}

int TextEditor::EditorState::GetLastAddedCursorIndex()
{
	return mLastAddedCursor > mCurrentCursor ? 0 : mLastAddedCursor;
}

void TextEditor::EditorState::SortCursorsFromTopToBottom()
{
	Coordinates lastAddedCursorPos = mCursors[GetLastAddedCursorIndex()].mInteractiveEnd;
	std::sort(mCursors.begin(), mCursors.begin() + (mCurrentCursor + 1), [](const Cursor& a, const Cursor& b) -> bool
		{
			return a.GetSelectionStart() < b.GetSelectionStart();
		});
	// update last added cursor index to be valid after sort
	for (int c = mCurrentCursor; c > -1; c--)
		if (mCursors[c].mInteractiveEnd == lastAddedCursorPos)
			mLastAddedCursor = c;
}

// ---------- Undo record functions --------- //

TextEditor::UndoRecord::UndoRecord(const std::vector<UndoOperation>& aOperations,
	TextEditor::EditorState& aBefore, TextEditor::EditorState& aAfter)
{
	mOperations = aOperations;
	mBefore = aBefore;
	mAfter = aAfter;
	for (const UndoOperation& o : mOperations)
		assert(o.mStart <= o.mEnd);
}

void TextEditor::UndoRecord::Undo(TextEditor* aEditor)
{
	for (int i = mOperations.size() - 1; i > -1; i--)
	{
		const UndoOperation& operation = mOperations[i];
		if (!operation.mText.empty())
		{
			switch (operation.mType)
			{
			case UndoOperationType::Delete:
			{
				auto start = operation.mStart;
				aEditor->InsertTextAt(start, operation.mText.c_str());
				aEditor->Colorize(operation.mStart.mLine - 1, operation.mEnd.mLine - operation.mStart.mLine + 2);
				break;
			}
			case UndoOperationType::Add:
			{
				aEditor->DeleteRange(operation.mStart, operation.mEnd);
				aEditor->Colorize(operation.mStart.mLine - 1, operation.mEnd.mLine - operation.mStart.mLine + 2);
				break;
			}
			}
		}
	}

	aEditor->mState = mBefore;
	aEditor->EnsureCursorVisible();
}

void TextEditor::UndoRecord::Redo(TextEditor* aEditor)
{
	for (int i = 0; i < mOperations.size(); i++)
	{
		const UndoOperation& operation = mOperations[i];
		if (!operation.mText.empty())
		{
			switch (operation.mType)
			{
			case UndoOperationType::Delete:
			{
				aEditor->DeleteRange(operation.mStart, operation.mEnd);
				aEditor->Colorize(operation.mStart.mLine - 1, operation.mEnd.mLine - operation.mStart.mLine + 1);
				break;
			}
			case UndoOperationType::Add:
			{
				auto start = operation.mStart;
				aEditor->InsertTextAt(start, operation.mText.c_str());
				aEditor->Colorize(operation.mStart.mLine - 1, operation.mEnd.mLine - operation.mStart.mLine + 1);
				break;
			}
			}
		}
	}

	aEditor->mState = mAfter;
	aEditor->EnsureCursorVisible();
}

// ---------- Text editor internal functions --------- //

std::string TextEditor::GetText(const Coordinates& aStart, const Coordinates& aEnd) const
{
	assert(aStart < aEnd);

	std::string result;
	auto lstart = aStart.mLine;
	auto lend = aEnd.mLine;
	auto istart = GetCharacterIndexR(aStart);
	auto iend = GetCharacterIndexR(aEnd);
	size_t s = 0;

	for (size_t i = lstart; i < lend; i++)
		s += mLines[i].size();

	result.reserve(s + s / 8);

	while (istart < iend || lstart < lend)
	{
		if (lstart >= (int)mLines.size())
			break;

		auto& line = mLines[lstart];
		if (istart < (int)line.size())
		{
			result += line[istart].mChar;
			istart++;
		}
		else
		{
			istart = 0;
			++lstart;
			result += '\n';
		}
	}

	return result;
}

std::string TextEditor::GetClipboardText() const
{
	std::string result;
	for (int c = 0; c <= mState.mCurrentCursor; c++)
	{
		if (mState.mCursors[c].GetSelectionStart() < mState.mCursors[c].GetSelectionEnd())
		{
			if (result.length() != 0)
				result += '\n';
			result += GetText(mState.mCursors[c].GetSelectionStart(), mState.mCursors[c].GetSelectionEnd());
		}
	}
	return result;
}

std::string TextEditor::GetSelectedText(int aCursor) const
{
	if (aCursor == -1)
		aCursor = mState.mCurrentCursor;

	if (!mState.mCursors[aCursor].HasSelection())
		return "";

	return GetText(mState.mCursors[aCursor].GetSelectionStart(), mState.mCursors[aCursor].GetSelectionEnd());
}

void TextEditor::SetCursorPosition(const Coordinates& aPosition, int aCursor, bool aClearSelection)
{
	if (aCursor == -1)
		aCursor = mState.mCurrentCursor;

	mCursorPositionChanged = true;
	if (aClearSelection)
		mState.mCursors[aCursor].mInteractiveStart = aPosition;
	if (mState.mCursors[aCursor].mInteractiveEnd != aPosition)
	{
		mState.mCursors[aCursor].mInteractiveEnd = aPosition;
		EnsureCursorVisible();
	}
}

int TextEditor::InsertTextAt(Coordinates& /* inout */ aWhere, const char* aValue)
{
	assert(!mReadOnly);
	MarkFindResultsDirty(true);
	mFindHighlightsCache.clear();

	int cindex = GetCharacterIndexR(aWhere);
	int totalLines = 0;
	while (*aValue != '\0')
	{
		assert(!mLines.empty());

		if (*aValue == '\r')
		{
			// skip
			++aValue;
		}
		else if (*aValue == '\n')
		{
			if (cindex < (int)mLines[aWhere.mLine].size())
			{
				auto& newLine = InsertLine(aWhere.mLine + 1);
				auto& line = mLines[aWhere.mLine];
				AddGlyphsToLine(aWhere.mLine + 1, 0, line.begin() + cindex, line.end());
				RemoveGlyphsFromLine(aWhere.mLine, cindex);
			}
			else
			{
				InsertLine(aWhere.mLine + 1);
			}
			++aWhere.mLine;
			aWhere.mColumn = 0;
			cindex = 0;
			++totalLines;
			++aValue;
		}
		else
		{
			auto& line = mLines[aWhere.mLine];
			auto d = UTF8CharLength(*aValue);
			while (d-- > 0 && *aValue != '\0')
				AddGlyphToLine(aWhere.mLine, cindex++, Glyph(*aValue++, PaletteIndex::Default));
			aWhere.mColumn = GetCharacterColumn(aWhere.mLine, cindex);
		}
	}

	return totalLines;
}

void TextEditor::InsertTextAtCursor(const char* aValue, int aCursor)
{
	if (aValue == nullptr)
		return;
	if (aCursor == -1)
		aCursor = mState.mCurrentCursor;

	auto pos = GetSanitizedCursorCoordinates(aCursor);
	auto start = std::min(pos, mState.mCursors[aCursor].GetSelectionStart());
	int totalLines = pos.mLine - start.mLine;

	totalLines += InsertTextAt(pos, aValue);

	SetCursorPosition(pos, aCursor);
	Colorize(start.mLine - 1, totalLines + 2);
}

bool TextEditor::Move(int& aLine, int& aCharIndex, bool aLeft, bool aLockLine) const
{
	// assumes given char index is not in the middle of utf8 sequence
	// char index can be line.length()

	// invalid line
	if (aLine >= mLines.size())
		return false;

	if (aLeft)
	{
		if (aCharIndex == 0)
		{
			if (aLockLine || aLine == 0)
				return false;
			aLine--;
			aCharIndex = mLines[aLine].size();
		}
		else
		{
			aCharIndex--;
			while (aCharIndex > 0 && IsUTFSequence(mLines[aLine][aCharIndex].mChar))
				aCharIndex--;
		}
	}
	else // right
	{
		if (aCharIndex == mLines[aLine].size())
		{
			if (aLockLine || aLine == mLines.size() - 1)
				return false;
			aLine++;
			aCharIndex = 0;
		}
		else
		{
			int seqLength = UTF8CharLength(mLines[aLine][aCharIndex].mChar);
			aCharIndex = std::min(aCharIndex + seqLength, (int)mLines[aLine].size());
		}
	}
	return true;
}

void TextEditor::MoveCharIndexAndColumn(int aLine, int& aCharIndex, int& aColumn) const
{
	assert(aLine < mLines.size());
	assert(aCharIndex < mLines[aLine].size());
	char c = mLines[aLine][aCharIndex].mChar;
	aCharIndex += UTF8CharLength(c);
	if (c == '\t')
		aColumn = (aColumn / mTabSize) * mTabSize + mTabSize;
	else
		aColumn++;
}

void TextEditor::MoveCoords(Coordinates& aCoords, MoveDirection aDirection, bool aWordMode, int aLineCount) const
{
	int charIndex = GetCharacterIndexR(aCoords);
	int lineIndex = aCoords.mLine;
	switch (aDirection)
	{
	case MoveDirection::Right:
		if (charIndex >= mLines[lineIndex].size())
		{
			if (lineIndex < mLines.size() - 1)
			{
				aCoords.mLine = std::max(0, std::min((int)mLines.size() - 1, lineIndex + 1));
				aCoords.mColumn = 0;
			}
		}
		else
		{
			Move(lineIndex, charIndex);
			int oneStepRightColumn = GetCharacterColumn(lineIndex, charIndex);
			if (aWordMode)
			{
				aCoords = FindWordEnd(aCoords);
				aCoords.mColumn = std::max(aCoords.mColumn, oneStepRightColumn);
			}
			else
				aCoords.mColumn = oneStepRightColumn;
		}
		break;
	case MoveDirection::Left:
		if (charIndex == 0)
		{
			if (lineIndex > 0)
			{
				aCoords.mLine = lineIndex - 1;
				aCoords.mColumn = GetLineMaxColumn(aCoords.mLine);
			}
		}
		else
		{
			Move(lineIndex, charIndex, true);
			aCoords.mColumn = GetCharacterColumn(lineIndex, charIndex);
			if (aWordMode)
				aCoords = FindWordStart(aCoords);
		}
		break;
	case MoveDirection::Up:
		aCoords.mLine = std::max(0, lineIndex - aLineCount);
		break;
	case MoveDirection::Down:
		aCoords.mLine = std::max(0, std::min((int)mLines.size() - 1, lineIndex + aLineCount));
		break;
	}
}

void TextEditor::MoveUp(int aAmount, bool aSelect)
{
	for (int c = 0; c <= mState.mCurrentCursor; c++)
	{
		Coordinates newCoords = mState.mCursors[c].mInteractiveEnd;
		MoveCoords(newCoords, MoveDirection::Up, false, aAmount);
		SetCursorPosition(newCoords, c, !aSelect);
	}
	EnsureCursorVisible();
}

void TextEditor::MoveDown(int aAmount, bool aSelect)
{
	for (int c = 0; c <= mState.mCurrentCursor; c++)
	{
		assert(mState.mCursors[c].mInteractiveEnd.mColumn >= 0);
		Coordinates newCoords = mState.mCursors[c].mInteractiveEnd;
		MoveCoords(newCoords, MoveDirection::Down, false, aAmount);
		SetCursorPosition(newCoords, c, !aSelect);
	}
	EnsureCursorVisible();
}

void TextEditor::MoveLeft(bool aSelect, bool aWordMode)
{
	if (mLines.empty())
		return;

	if (AnyCursorHasSelection() && !aSelect && !aWordMode)
	{
		for (int c = 0; c <= mState.mCurrentCursor; c++)
			SetCursorPosition(mState.mCursors[c].GetSelectionStart(), c);
	}
	else
	{
		for (int c = 0; c <= mState.mCurrentCursor; c++)
		{
			Coordinates newCoords = mState.mCursors[c].mInteractiveEnd;
			MoveCoords(newCoords, MoveDirection::Left, aWordMode);
			SetCursorPosition(newCoords, c, !aSelect);
		}
	}
	EnsureCursorVisible();
}

void TextEditor::MoveRight(bool aSelect, bool aWordMode)
{
	if (mLines.empty())
		return;

	if (AnyCursorHasSelection() && !aSelect && !aWordMode)
	{
		for (int c = 0; c <= mState.mCurrentCursor; c++)
			SetCursorPosition(mState.mCursors[c].GetSelectionEnd(), c);
	}
	else
	{
		for (int c = 0; c <= mState.mCurrentCursor; c++)
		{
			Coordinates newCoords = mState.mCursors[c].mInteractiveEnd;
			MoveCoords(newCoords, MoveDirection::Right, aWordMode);
			SetCursorPosition(newCoords, c, !aSelect);
		}
	}
	EnsureCursorVisible();
}

void TextEditor::MoveTop(bool aSelect)
{
	SetCursorPosition(Coordinates(0, 0), mState.mCurrentCursor, !aSelect);
}

void TextEditor::TextEditor::MoveBottom(bool aSelect)
{
	int maxLine = (int)mLines.size() - 1;
	Coordinates newPos = Coordinates(maxLine, GetLineMaxColumn(maxLine));
	SetCursorPosition(newPos, mState.mCurrentCursor, !aSelect);
}

void TextEditor::MoveHome(bool aSelect)
{
	for (int c = 0; c <= mState.mCurrentCursor; c++)
		SetCursorPosition(Coordinates(mState.mCursors[c].mInteractiveEnd.mLine, 0), c, !aSelect);
}

void TextEditor::MoveEnd(bool aSelect)
{
	for (int c = 0; c <= mState.mCurrentCursor; c++)
	{
		int lindex = mState.mCursors[c].mInteractiveEnd.mLine;
		SetCursorPosition(Coordinates(lindex, GetLineMaxColumn(lindex)), c, !aSelect);
	}
}

void TextEditor::EnterCharacter(ImWchar aChar, bool aShift)
{
	assert(!mReadOnly);

	bool hasSelection = AnyCursorHasSelection();
	bool anyCursorHasMultilineSelection = false;
	for (int c = mState.mCurrentCursor; c > -1; c--)
		if (mState.mCursors[c].GetSelectionStart().mLine != mState.mCursors[c].GetSelectionEnd().mLine)
		{
			anyCursorHasMultilineSelection = true;
			break;
		}
	bool isIndentOperation = hasSelection && anyCursorHasMultilineSelection && aChar == '\t';
	if (isIndentOperation)
	{
		ChangeCurrentLinesIndentation(!aShift);
		return;
	}

	UndoRecord u;
	u.mBefore = mState;

	if (hasSelection)
	{
		for (int c = mState.mCurrentCursor; c > -1; c--)
		{
			u.mOperations.push_back({ GetSelectedText(c), mState.mCursors[c].GetSelectionStart(), mState.mCursors[c].GetSelectionEnd(), UndoOperationType::Delete });
			DeleteSelection(c);
		}
	}

	std::vector<Coordinates> coords;
	for (int c = mState.mCurrentCursor; c > -1; c--) // order important here for typing \n in the same line at the same time
	{
		auto coord = GetSanitizedCursorCoordinates(c);
		coords.push_back(coord);
		UndoOperation added;
		added.mType = UndoOperationType::Add;
		added.mStart = coord;

		assert(!mLines.empty());

		if (aChar == '\n')
		{
			InsertLine(coord.mLine + 1);
			auto& line = mLines[coord.mLine];
			auto& newLine = mLines[coord.mLine + 1];

			added.mText = "";
			added.mText += (char)aChar;
			if (mAutoIndent)
				for (int i = 0; i < line.size() && isascii(line[i].mChar) && isblank(line[i].mChar); ++i)
				{
					newLine.push_back(line[i]);
					added.mText += line[i].mChar;
				}

			const size_t whitespaceSize = newLine.size();
			auto cindex = GetCharacterIndexR(coord);
			AddGlyphsToLine(coord.mLine + 1, newLine.size(), line.begin() + cindex, line.end());
			RemoveGlyphsFromLine(coord.mLine, cindex);
			SetCursorPosition(Coordinates(coord.mLine + 1, GetCharacterColumn(coord.mLine + 1, (int)whitespaceSize)), c);
		}
		else
		{
			char buf[7];
			int e = ImTextCharToUtf8(buf, 7, aChar);
			if (e > 0)
			{
				buf[e] = '\0';
				auto& line = mLines[coord.mLine];
				auto cindex = GetCharacterIndexR(coord);

				for (auto p = buf; *p != '\0'; p++, ++cindex)
					AddGlyphToLine(coord.mLine, cindex, Glyph(*p, PaletteIndex::Default));
				added.mText = buf;

				SetCursorPosition(Coordinates(coord.mLine, GetCharacterColumn(coord.mLine, cindex)), c);
			}
			else
				continue;
		}

		added.mEnd = GetSanitizedCursorCoordinates(c);
		u.mOperations.push_back(added);
	}

	u.mAfter = mState;
	AddUndo(u);

	for (const auto& coord : coords)
		Colorize(coord.mLine - 1, 3);
	EnsureCursorVisible();
}

void TextEditor::Backspace(bool aWordMode)
{
	assert(!mReadOnly);

	if (mLines.empty())
		return;

	if (AnyCursorHasSelection())
		Delete(aWordMode);
	else
	{
		EditorState stateBeforeDeleting = mState;
		MoveLeft(true, aWordMode);
		if (!AllCursorsHaveSelection()) // can't do backspace if any cursor at {0,0}
		{
			if (AnyCursorHasSelection())
				MoveRight();
			return;
		}
			
		OnCursorPositionChanged(); // might combine cursors
		Delete(aWordMode, &stateBeforeDeleting);
	}
}

void TextEditor::Delete(bool aWordMode, const EditorState* aEditorState)
{
	assert(!mReadOnly);

	if (mLines.empty())
		return;

	if (AnyCursorHasSelection())
	{
		UndoRecord u;
		u.mBefore = aEditorState == nullptr ? mState : *aEditorState;
		for (int c = mState.mCurrentCursor; c > -1; c--)
		{
			if (!mState.mCursors[c].HasSelection())
				continue;
			u.mOperations.push_back({ GetSelectedText(c), mState.mCursors[c].GetSelectionStart(), mState.mCursors[c].GetSelectionEnd(), UndoOperationType::Delete });
			DeleteSelection(c);
		}
		u.mAfter = mState;
		AddUndo(u);
	}
	else
	{
		EditorState stateBeforeDeleting = mState;
		MoveRight(true, aWordMode);
		if (!AllCursorsHaveSelection()) // can't do delete if any cursor at end of last line
		{
			if (AnyCursorHasSelection())
				MoveLeft();
			return;
		}

		OnCursorPositionChanged(); // might combine cursors
		Delete(aWordMode, &stateBeforeDeleting);
	}
}

void TextEditor::SetSelection(Coordinates aStart, Coordinates aEnd, int aCursor)
{
	if (aCursor == -1)
		aCursor = mState.mCurrentCursor;

	Coordinates minCoords = Coordinates(0, 0);
	int maxLine = (int)mLines.size() - 1;
	Coordinates maxCoords = Coordinates(maxLine, GetLineMaxColumn(maxLine));
	if (aStart < minCoords)
		aStart = minCoords;
	else if (aStart > maxCoords)
		aStart = maxCoords;
	if (aEnd < minCoords)
		aEnd = minCoords;
	else if (aEnd > maxCoords)
		aEnd = maxCoords;

	mState.mCursors[aCursor].mInteractiveStart = aStart;
	SetCursorPosition(aEnd, aCursor, false);
}

void TextEditor::SetSelection(int aStartLine, int aStartChar, int aEndLine, int aEndChar, int aCursor)
{
	Coordinates startCoords = { aStartLine, GetCharacterColumn(aStartLine, aStartChar) };
	Coordinates endCoords = { aEndLine, GetCharacterColumn(aEndLine, aEndChar) };
	SetSelection(startCoords, endCoords, aCursor);
}

void TextEditor::SelectNextOccurrenceOf(const char* aText, int aTextSize, int aCursor, bool aCaseSensitive)
{
	if (aCursor == -1)
		aCursor = mState.mCurrentCursor;
	Coordinates nextStart, nextEnd;
	FindNextOccurrence(aText, aTextSize, mState.mCursors[aCursor].mInteractiveEnd, nextStart, nextEnd, aCaseSensitive);
	SetSelection(nextStart, nextEnd, aCursor);
	EnsureCursorVisible(aCursor, true);
}

void TextEditor::AddCursorForNextOccurrence(bool aCaseSensitive)
{
	const Cursor& currentCursor = mState.mCursors[mState.GetLastAddedCursorIndex()];
	if (currentCursor.GetSelectionStart() == currentCursor.GetSelectionEnd())
		return;

	std::string selectionText = GetText(currentCursor.GetSelectionStart(), currentCursor.GetSelectionEnd());
	Coordinates nextStart, nextEnd;
	if (!FindNextOccurrence(selectionText.c_str(), selectionText.length(), currentCursor.GetSelectionEnd(), nextStart, nextEnd, aCaseSensitive))
		return;

	mState.AddCursor();
	SetSelection(nextStart, nextEnd, mState.mCurrentCursor);
	mState.SortCursorsFromTopToBottom();
	MergeCursorsIfPossible();
	EnsureCursorVisible(-1, true);
}

bool TextEditor::FindNextOccurrence(const char* aText, int aTextSize, const Coordinates& aFrom, Coordinates& outStart, Coordinates& outEnd, bool aCaseSensitive)
{
	assert(aTextSize > 0);
	bool fmatches = false;
	int fline, ifline;
	int findex, ifindex;

	ifline = fline = aFrom.mLine;
	ifindex = findex = GetCharacterIndexR(aFrom);
	
	// Track if we've started from the beginning (used to prevent wrapping in linear search)
	bool startedFromBeginning = (ifline == 0 && ifindex == 0);
	bool hasProcessedStart = false;

	while (true)
	{
		bool matches;
		{ // match function
			int lineOffset = 0;
			int currentCharIndex = findex;
			int i = 0;
			for (; i < aTextSize; i++)
			{
				if (currentCharIndex == mLines[fline + lineOffset].size())
				{
					if (aText[i] == '\n' && fline + lineOffset + 1 < mLines.size())
					{
						currentCharIndex = 0;
						lineOffset++;
					}
					else
						break;
				}
				else
				{
					char toCompareA = mLines[fline + lineOffset][currentCharIndex].mChar;
					char toCompareB = aText[i];
					toCompareA = (!aCaseSensitive && toCompareA >= 'A' && toCompareA <= 'Z') ? toCompareA - 'A' + 'a' : toCompareA;
					toCompareB = (!aCaseSensitive && toCompareB >= 'A' && toCompareB <= 'Z') ? toCompareB - 'A' + 'a' : toCompareB;
					if (toCompareA != toCompareB)
						break;
					else
						currentCharIndex++;
				}
			}
			matches = i == aTextSize;
			if (matches)
			{
				outStart = { fline, GetCharacterColumn(fline, findex) };
				outEnd = { fline + lineOffset, GetCharacterColumn(fline + lineOffset, currentCharIndex) };
				return true;
			}
		}

		// move forward
		if (findex == mLines[fline].size()) // need to consider line breaks
		{
			if (fline == mLines.size() - 1)
			{
				// If we started from beginning, don't wrap (linear search for all occurrences)
				if (startedFromBeginning)
					return false;
				
				// For non-linear search (Find Next), wrap around once
				fline = 0;
				findex = 0;
			}
			else
			{
				fline++;
				findex = 0;
			}
		}
		else
			findex++;

		// For wrapped search, detect when we've completed the scan
		if (!startedFromBeginning)
		{
			// Mark that we've processed the starting position
			if (fline == ifline && findex == ifindex)
				hasProcessedStart = true;
			
			// If we've wrapped and reached or passed the start position after processing it once
			if (hasProcessedStart && ((fline == ifline && findex == ifindex) || 
			    (fline == 0 && findex == 0 && ifline == 0 && ifindex == 0)))
				return false;
		}
	}

	return false;
}

bool TextEditor::FindMatchingBracket(int aLine, int aCharIndex, Coordinates& out)
{
	if (aLine > mLines.size() - 1)
		return false;
	int maxCharIndex = mLines[aLine].size() - 1;
	if (aCharIndex > maxCharIndex)
		return false;

	int currentLine = aLine;
	int currentCharIndex = aCharIndex;
	int counter = 1;
	if (CLOSE_TO_OPEN_CHAR.find(mLines[aLine][aCharIndex].mChar) != CLOSE_TO_OPEN_CHAR.end())
	{
		char closeChar = mLines[aLine][aCharIndex].mChar;
		char openChar = CLOSE_TO_OPEN_CHAR.at(closeChar);
		while (Move(currentLine, currentCharIndex, true))
		{
			if (currentCharIndex < mLines[currentLine].size())
			{
				char currentChar = mLines[currentLine][currentCharIndex].mChar;
				if (currentChar == openChar)
				{
					counter--;
					if (counter == 0)
					{
						out = { currentLine, GetCharacterColumn(currentLine, currentCharIndex) };
						return true;
					}
				}
				else if (currentChar == closeChar)
					counter++;
			}
		}
	}
	else if (OPEN_TO_CLOSE_CHAR.find(mLines[aLine][aCharIndex].mChar) != OPEN_TO_CLOSE_CHAR.end())
	{
		char openChar = mLines[aLine][aCharIndex].mChar;
		char closeChar = OPEN_TO_CLOSE_CHAR.at(openChar);
		while (Move(currentLine, currentCharIndex))
		{
			if (currentCharIndex < mLines[currentLine].size())
			{
				char currentChar = mLines[currentLine][currentCharIndex].mChar;
				if (currentChar == closeChar)
				{
					counter--;
					if (counter == 0)
					{
						out = { currentLine, GetCharacterColumn(currentLine, currentCharIndex) };
						return true;
					}
				}
				else if (currentChar == openChar)
					counter++;
			}
		}
	}
	return false;
}

void TextEditor::ChangeCurrentLinesIndentation(bool aIncrease)
{
	assert(!mReadOnly);

	UndoRecord u;
	u.mBefore = mState;

	for (int c = mState.mCurrentCursor; c > -1; c--)
	{
		for (int currentLine = mState.mCursors[c].GetSelectionEnd().mLine; currentLine >= mState.mCursors[c].GetSelectionStart().mLine; currentLine--)
		{
			if (Coordinates{ currentLine, 0 } == mState.mCursors[c].GetSelectionEnd() && mState.mCursors[c].GetSelectionEnd() != mState.mCursors[c].GetSelectionStart()) // when selection ends at line start
				continue;

			if (aIncrease)
			{
				if (mLines[currentLine].size() > 0)
				{
					Coordinates lineStart = { currentLine, 0 };
					Coordinates insertionEnd = lineStart;
					InsertTextAt(insertionEnd, "\t"); // sets insertion end
					u.mOperations.push_back({ "\t", lineStart, insertionEnd, UndoOperationType::Add });
					Colorize(lineStart.mLine, 1);
				}
			}
			else
			{
				Coordinates start = { currentLine, 0 };
				Coordinates end = { currentLine, mTabSize };
				int charIndex = GetCharacterIndexL(end) - 1;
				while (charIndex > -1 && (mLines[currentLine][charIndex].mChar == ' ' || mLines[currentLine][charIndex].mChar == '\t')) charIndex--;
				bool onlySpaceCharactersFound = charIndex == -1;
				if (onlySpaceCharactersFound)
				{
					u.mOperations.push_back({ GetText(start, end), start, end, UndoOperationType::Delete });
					DeleteRange(start, end);
					Colorize(currentLine, 1);
				}
			}
		}
	}

	if (u.mOperations.size() > 0)
		AddUndo(u);
}

void TextEditor::MoveUpCurrentLines()
{
	assert(!mReadOnly);

	UndoRecord u;
	u.mBefore = mState;

	std::set<int> affectedLines;
	int minLine = -1;
	int maxLine = -1;
	for (int c = mState.mCurrentCursor; c > -1; c--) // cursors are expected to be sorted from top to bottom
	{
		for (int currentLine = mState.mCursors[c].GetSelectionEnd().mLine; currentLine >= mState.mCursors[c].GetSelectionStart().mLine; currentLine--)
		{
			if (Coordinates{ currentLine, 0 } == mState.mCursors[c].GetSelectionEnd() && mState.mCursors[c].GetSelectionEnd() != mState.mCursors[c].GetSelectionStart()) // when selection ends at line start
				continue;
			affectedLines.insert(currentLine);
			minLine = minLine == -1 ? currentLine : (currentLine < minLine ? currentLine : minLine);
			maxLine = maxLine == -1 ? currentLine : (currentLine > maxLine ? currentLine : maxLine);
		}
	}
	if (minLine == 0) // can't move up anymore
		return;

	Coordinates start = { minLine - 1, 0 };
	Coordinates end = { maxLine, GetLineMaxColumn(maxLine) };
	u.mOperations.push_back({ GetText(start, end), start, end, UndoOperationType::Delete });

	for (int line : affectedLines) // lines should be sorted here
		std::swap(mLines[line - 1], mLines[line]);
	for (int c = mState.mCurrentCursor; c > -1; c--)
	{
		mState.mCursors[c].mInteractiveStart.mLine -= 1;
		mState.mCursors[c].mInteractiveEnd.mLine -= 1;
		// no need to set mCursorPositionChanged as cursors will remain sorted
	}

	end = { maxLine, GetLineMaxColumn(maxLine) }; // this line is swapped with line above, need to find new max column
	u.mOperations.push_back({ GetText(start, end), start, end, UndoOperationType::Add });
	u.mAfter = mState;
	AddUndo(u);
}

void TextEditor::MoveDownCurrentLines()
{
	assert(!mReadOnly);

	UndoRecord u;
	u.mBefore = mState;

	std::set<int> affectedLines;
	int minLine = -1;
	int maxLine = -1;
	for (int c = 0; c <= mState.mCurrentCursor; c++) // cursors are expected to be sorted from top to bottom
	{
		for (int currentLine = mState.mCursors[c].GetSelectionEnd().mLine; currentLine >= mState.mCursors[c].GetSelectionStart().mLine; currentLine--)
		{
			if (Coordinates{ currentLine, 0 } == mState.mCursors[c].GetSelectionEnd() && mState.mCursors[c].GetSelectionEnd() != mState.mCursors[c].GetSelectionStart()) // when selection ends at line start
				continue;
			affectedLines.insert(currentLine);
			minLine = minLine == -1 ? currentLine : (currentLine < minLine ? currentLine : minLine);
			maxLine = maxLine == -1 ? currentLine : (currentLine > maxLine ? currentLine : maxLine);
		}
	}
	if (maxLine == mLines.size() - 1) // can't move down anymore
		return;

	Coordinates start = { minLine, 0 };
	Coordinates end = { maxLine + 1, GetLineMaxColumn(maxLine + 1)};
	u.mOperations.push_back({ GetText(start, end), start, end, UndoOperationType::Delete });

	std::set<int>::reverse_iterator rit;
	for (rit = affectedLines.rbegin(); rit != affectedLines.rend(); rit++) // lines should be sorted here
		std::swap(mLines[*rit + 1], mLines[*rit]);
	for (int c = mState.mCurrentCursor; c > -1; c--)
	{
		mState.mCursors[c].mInteractiveStart.mLine += 1;
		mState.mCursors[c].mInteractiveEnd.mLine += 1;
		// no need to set mCursorPositionChanged as cursors will remain sorted
	}

	end = { maxLine + 1, GetLineMaxColumn(maxLine + 1) }; // this line is swapped with line below, need to find new max column
	u.mOperations.push_back({ GetText(start, end), start, end, UndoOperationType::Add });
	u.mAfter = mState;
	AddUndo(u);
}

void TextEditor::ToggleLineComment()
{
	assert(!mReadOnly);
	if (mLanguageDefinition == nullptr)
		return;
	const std::string& commentString = mLanguageDefinition->mSingleLineComment;

	UndoRecord u;
	u.mBefore = mState;

	bool shouldAddComment = false;
	std::unordered_set<int> affectedLines;
	for (int c = mState.mCurrentCursor; c > -1; c--)
	{
		for (int currentLine = mState.mCursors[c].GetSelectionEnd().mLine; currentLine >= mState.mCursors[c].GetSelectionStart().mLine; currentLine--)
		{
			if (Coordinates{ currentLine, 0 } == mState.mCursors[c].GetSelectionEnd() && mState.mCursors[c].GetSelectionEnd() != mState.mCursors[c].GetSelectionStart()) // when selection ends at line start
				continue;
			affectedLines.insert(currentLine);
			int currentIndex = 0;
			while (currentIndex < mLines[currentLine].size() && (mLines[currentLine][currentIndex].mChar == ' ' || mLines[currentLine][currentIndex].mChar == '\t')) currentIndex++;
			if (currentIndex == mLines[currentLine].size())
				continue;
			int i = 0;
			while (i < commentString.length() && currentIndex + i < mLines[currentLine].size() && mLines[currentLine][currentIndex + i].mChar == commentString[i]) i++;
			bool matched = i == commentString.length();
			shouldAddComment |= !matched;
		}
	}

	if (shouldAddComment)
	{
		for (int currentLine : affectedLines) // order doesn't matter as changes are not multiline
		{
			Coordinates lineStart = { currentLine, 0 };
			Coordinates insertionEnd = lineStart;
			InsertTextAt(insertionEnd, (commentString + ' ').c_str()); // sets insertion end
			u.mOperations.push_back({ (commentString + ' ') , lineStart, insertionEnd, UndoOperationType::Add });
			Colorize(lineStart.mLine, 1);
		}
	}
	else
	{
		for (int currentLine : affectedLines) // order doesn't matter as changes are not multiline
		{
			int currentIndex = 0;
			while (currentIndex < mLines[currentLine].size() && (mLines[currentLine][currentIndex].mChar == ' ' || mLines[currentLine][currentIndex].mChar == '\t')) currentIndex++;
			if (currentIndex == mLines[currentLine].size())
				continue;
			int i = 0;
			while (i < commentString.length() && currentIndex + i < mLines[currentLine].size() && mLines[currentLine][currentIndex + i].mChar == commentString[i]) i++;
			bool matched = i == commentString.length();
			assert(matched);
			if (currentIndex + i < mLines[currentLine].size() && mLines[currentLine][currentIndex + i].mChar == ' ')
				i++;

			Coordinates start = { currentLine, GetCharacterColumn(currentLine, currentIndex) };
			Coordinates end = { currentLine, GetCharacterColumn(currentLine, currentIndex + i) };
			u.mOperations.push_back({ GetText(start, end) , start, end, UndoOperationType::Delete});
			DeleteRange(start, end);
			Colorize(currentLine, 1);
		}
	}

	u.mAfter = mState;
	AddUndo(u);
}

void TextEditor::RemoveCurrentLines()
{
	UndoRecord u;
	u.mBefore = mState;

	if (AnyCursorHasSelection())
	{
		for (int c = mState.mCurrentCursor; c > -1; c--)
		{
			if (!mState.mCursors[c].HasSelection())
				continue;
			u.mOperations.push_back({ GetSelectedText(c), mState.mCursors[c].GetSelectionStart(), mState.mCursors[c].GetSelectionEnd(), UndoOperationType::Delete });
			DeleteSelection(c);
		}
	}
	MoveHome();
	OnCursorPositionChanged(); // might combine cursors

	for (int c = mState.mCurrentCursor; c > -1; c--)
	{
		int currentLine = mState.mCursors[c].mInteractiveEnd.mLine;
		int nextLine = currentLine + 1;
		int prevLine = currentLine - 1;

		Coordinates toDeleteStart, toDeleteEnd;
		if (mLines.size() > nextLine) // next line exists
		{
			toDeleteStart = Coordinates(currentLine, 0);
			toDeleteEnd = Coordinates(nextLine, 0);
			SetCursorPosition({ mState.mCursors[c].mInteractiveEnd.mLine, 0 }, c);
		}
		else if (prevLine > -1) // previous line exists
		{
			toDeleteStart = Coordinates(prevLine, GetLineMaxColumn(prevLine));
			toDeleteEnd = Coordinates(currentLine, GetLineMaxColumn(currentLine));
			SetCursorPosition({ prevLine, 0 }, c);
		}
		else
		{
			toDeleteStart = Coordinates(currentLine, 0);
			toDeleteEnd = Coordinates(currentLine, GetLineMaxColumn(currentLine));
			SetCursorPosition({ currentLine, 0 }, c);
		}

		u.mOperations.push_back({ GetText(toDeleteStart, toDeleteEnd), toDeleteStart, toDeleteEnd, UndoOperationType::Delete });

		std::unordered_set<int> handledCursors = { c };
		if (toDeleteStart.mLine != toDeleteEnd.mLine)
			RemoveLine(currentLine, &handledCursors);
		else
			DeleteRange(toDeleteStart, toDeleteEnd);
	}

	u.mAfter = mState;
	AddUndo(u);
}

float TextEditor::TextDistanceToLineStart(const Coordinates& aFrom, bool aSanitizeCoords) const
{
	if (aSanitizeCoords)
		return SanitizeCoordinates(aFrom).mColumn * mCharAdvance.x;
	else
		return aFrom.mColumn * mCharAdvance.x;
}

void TextEditor::EnsureCursorVisible(int aCursor, bool aStartToo)
{
	if (aCursor == -1)
		aCursor = mState.GetLastAddedCursorIndex();

	mEnsureCursorVisible = aCursor;
	mEnsureCursorVisibleStartToo = aStartToo;
	return;
}

TextEditor::Coordinates TextEditor::SanitizeCoordinates(const Coordinates& aValue) const
{
	// Clamp in document and line limits
	auto line = Max(aValue.mLine, 0);
	auto column = Max(aValue.mColumn, 0);
	Coordinates out;
	if (line >= (int) mLines.size())
	{
		if (mLines.empty())
		{
			line = 0;
			column = 0;
		}
		else
		{
			line = (int) mLines.size() - 1;
			column = GetLineMaxColumn(line);
		}
		out = Coordinates(line, column);
	}
	else
	{
		column = mLines.empty() ? 0 : GetLineMaxColumn(line, column);
		out = Coordinates(line, column);
	}

	// Move if inside a tab character
	int charIndex = GetCharacterIndexL(out);
	if (charIndex > -1 && charIndex < mLines[out.mLine].size() && mLines[out.mLine][charIndex].mChar == '\t')
	{
		int columnToLeft = GetCharacterColumn(out.mLine, charIndex);
		int columnToRight = GetCharacterColumn(out.mLine, GetCharacterIndexR(out));
		if (out.mColumn - columnToLeft <= columnToRight - out.mColumn)
			out.mColumn = columnToLeft;
		else
			out.mColumn = columnToRight;
	}
	return out;
}

TextEditor::Coordinates TextEditor::GetSanitizedCursorCoordinates(int aCursor, bool aStart) const
{
	aCursor = aCursor == -1 ? mState.mCurrentCursor : aCursor;
	return SanitizeCoordinates(aStart ? mState.mCursors[aCursor].mInteractiveStart : mState.mCursors[aCursor].mInteractiveEnd);
}

TextEditor::Coordinates TextEditor::ScreenPosToCoordinates(const ImVec2& aPosition, bool* isOverLineNumber) const
{
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImVec2 local(aPosition.x - origin.x + 3.0f, aPosition.y - origin.y);

	if (isOverLineNumber != nullptr)
		*isOverLineNumber = local.x < mTextStart;

	Coordinates out = {
		Max(0, (int)floor(local.y / mCharAdvance.y)),
		Max(0, (int)floor((local.x - mTextStart) / mCharAdvance.x))
	};
	out.mColumn = Max(0, (int)floor((local.x - mTextStart + POS_TO_COORDS_COLUMN_OFFSET * mCharAdvance.x) / mCharAdvance.x));

	return SanitizeCoordinates(out);
}

TextEditor::Coordinates TextEditor::FindWordStart(const Coordinates& aFrom) const
{
	if (aFrom.mLine >= (int)mLines.size())
		return aFrom;

	int lineIndex = aFrom.mLine;
	auto& line = mLines[lineIndex];
	int charIndex = GetCharacterIndexL(aFrom);

	if (charIndex > (int)line.size() || line.size() == 0)
		return aFrom;
	if (charIndex == (int)line.size())
		charIndex--;

	bool initialIsWordChar = CharIsWordChar(line[charIndex].mChar);
	bool initialIsSpace = isspace(line[charIndex].mChar);
	char initialChar = line[charIndex].mChar;
	while (Move(lineIndex, charIndex, true, true))
	{
		bool isWordChar = CharIsWordChar(line[charIndex].mChar);
		bool isSpace = isspace(line[charIndex].mChar);
		if (initialIsSpace && !isSpace ||
			initialIsWordChar && !isWordChar ||
			!initialIsWordChar && !initialIsSpace && initialChar != line[charIndex].mChar)
		{
			Move(lineIndex, charIndex, false, true); // one step to the right
			break;
		}
	}
	return { aFrom.mLine, GetCharacterColumn(aFrom.mLine, charIndex) };
}

TextEditor::Coordinates TextEditor::FindWordEnd(const Coordinates& aFrom) const
{
	if (aFrom.mLine >= (int)mLines.size())
		return aFrom;

	int lineIndex = aFrom.mLine;
	auto& line = mLines[lineIndex];
	auto charIndex = GetCharacterIndexL(aFrom);

	if (charIndex >= (int)line.size())
		return aFrom;

	bool initialIsWordChar = CharIsWordChar(line[charIndex].mChar);
	bool initialIsSpace = isspace(line[charIndex].mChar);
	char initialChar = line[charIndex].mChar;
	while (Move(lineIndex, charIndex, false, true))
	{
		if (charIndex == line.size())
			break;
		bool isWordChar = CharIsWordChar(line[charIndex].mChar);
		bool isSpace = isspace(line[charIndex].mChar);
		if (initialIsSpace && !isSpace ||
			initialIsWordChar && !isWordChar ||
			!initialIsWordChar && !initialIsSpace && initialChar != line[charIndex].mChar)
			break;
	}
	return { lineIndex, GetCharacterColumn(aFrom.mLine, charIndex) };
}

int TextEditor::GetCharacterIndexL(const Coordinates& aCoords) const
{
	if (aCoords.mLine >= mLines.size())
		return -1;

	auto& line = mLines[aCoords.mLine];
	int c = 0;
	int i = 0;
	int tabCoordsLeft = 0;

	for (; i < line.size() && c < aCoords.mColumn;)
	{
		if (line[i].mChar == '\t')
		{
			if (tabCoordsLeft == 0)
				tabCoordsLeft = TabSizeAtColumn(c);
			if (tabCoordsLeft > 0)
				tabCoordsLeft--;
			c++;
		}
		else
			++c;
		if (tabCoordsLeft == 0)
			i += UTF8CharLength(line[i].mChar);
	}
	return i;
}

int TextEditor::GetCharacterIndexR(const Coordinates& aCoords) const
{
	if (aCoords.mLine >= mLines.size())
		return -1;
	int c = 0;
	int i = 0;
	for (; i < mLines[aCoords.mLine].size() && c < aCoords.mColumn;)
		MoveCharIndexAndColumn(aCoords.mLine, i, c);
	return i;
}

int TextEditor::GetCharacterColumn(int aLine, int aIndex) const
{
	if (aLine >= mLines.size())
		return 0;
	int c = 0;
	int i = 0;
	while (i < aIndex && i < mLines[aLine].size())
		MoveCharIndexAndColumn(aLine, i, c);
	return c;
}

int TextEditor::GetFirstVisibleCharacterIndex(int aLine) const
{
	if (aLine >= mLines.size())
		return 0;
	int c = 0;
	int i = 0;
	while (c < mFirstVisibleColumn && i < mLines[aLine].size())
		MoveCharIndexAndColumn(aLine, i, c);
	if (c > mFirstVisibleColumn)
		i--;
	return i;
}

int TextEditor::GetLineMaxColumn(int aLine, int aLimit) const
{
	if (aLine >= mLines.size())
		return 0;
	int c = 0;
	if (aLimit == -1)
	{
		for (int i = 0; i < mLines[aLine].size(); )
			MoveCharIndexAndColumn(aLine, i, c);
	}
	else
	{
		for (int i = 0; i < mLines[aLine].size(); )
		{
			MoveCharIndexAndColumn(aLine, i, c);
			if (c > aLimit)
				return aLimit;
		}
	}
	return c;
}

TextEditor::Line& TextEditor::InsertLine(int aIndex)
{
	assert(!mReadOnly);
	auto& result = *mLines.insert(mLines.begin() + aIndex, Line());

	for (int c = 0; c <= mState.mCurrentCursor; c++) // handle multiple cursors
	{
		if (mState.mCursors[c].mInteractiveEnd.mLine >= aIndex)
			SetCursorPosition({ mState.mCursors[c].mInteractiveEnd.mLine + 1, mState.mCursors[c].mInteractiveEnd.mColumn }, c);
	}

	return result;
}

void TextEditor::RemoveLine(int aIndex, const std::unordered_set<int>* aHandledCursors)
{
	assert(!mReadOnly);
	assert(mLines.size() > 1);

	mLines.erase(mLines.begin() + aIndex);
	assert(!mLines.empty());

	// handle multiple cursors
	for (int c = 0; c <= mState.mCurrentCursor; c++)
	{
		if (mState.mCursors[c].mInteractiveEnd.mLine >= aIndex)
		{
			if (aHandledCursors == nullptr || aHandledCursors->find(c) == aHandledCursors->end()) // move up if has not been handled already
				SetCursorPosition({ mState.mCursors[c].mInteractiveEnd.mLine - 1, mState.mCursors[c].mInteractiveEnd.mColumn }, c);
		}
	}
}

void TextEditor::RemoveLines(int aStart, int aEnd)
{
	assert(!mReadOnly);
	assert(aEnd >= aStart);
	assert(mLines.size() > (size_t)(aEnd - aStart));

	mLines.erase(mLines.begin() + aStart, mLines.begin() + aEnd);
	assert(!mLines.empty());

	// handle multiple cursors
	for (int c = 0; c <= mState.mCurrentCursor; c++)
	{
		if (mState.mCursors[c].mInteractiveEnd.mLine >= aStart)
		{
			int targetLine = mState.mCursors[c].mInteractiveEnd.mLine - (aEnd - aStart);
			targetLine = targetLine < 0 ? 0 : targetLine;
			mState.mCursors[c].mInteractiveEnd.mLine = targetLine;
		}
		if (mState.mCursors[c].mInteractiveStart.mLine >= aStart)
		{
			int targetLine = mState.mCursors[c].mInteractiveStart.mLine - (aEnd - aStart);
			targetLine = targetLine < 0 ? 0 : targetLine;
			mState.mCursors[c].mInteractiveStart.mLine = targetLine;
		}
	}
}

void TextEditor::DeleteRange(const Coordinates& aStart, const Coordinates& aEnd)
{
	assert(aEnd >= aStart);
	assert(!mReadOnly);
	MarkFindResultsDirty(true);
	mFindHighlightsCache.clear();

	if (aEnd == aStart)
		return;

	auto start = GetCharacterIndexL(aStart);
	auto end = GetCharacterIndexR(aEnd);

	if (aStart.mLine == aEnd.mLine)
	{
		auto n = GetLineMaxColumn(aStart.mLine);
		if (aEnd.mColumn >= n)
			RemoveGlyphsFromLine(aStart.mLine, start); // from start to end of line
		else
			RemoveGlyphsFromLine(aStart.mLine, start, end);
	}
	else
	{
		RemoveGlyphsFromLine(aStart.mLine, start); // from start to end of line
		RemoveGlyphsFromLine(aEnd.mLine, 0, end);
		auto& firstLine = mLines[aStart.mLine];
		auto& lastLine = mLines[aEnd.mLine];

		if (aStart.mLine < aEnd.mLine)
		{
			AddGlyphsToLine(aStart.mLine, firstLine.size(), lastLine.begin(), lastLine.end());
			for (int c = 0; c <= mState.mCurrentCursor; c++) // move up cursors in line that is being moved up
			{
				// if cursor is selecting the same range we are deleting, it's because this is being called from
				// DeleteSelection which already sets the cursor position after the range is deleted
				if (mState.mCursors[c].GetSelectionStart() == aStart && mState.mCursors[c].GetSelectionEnd() == aEnd)
					continue;
				if (mState.mCursors[c].mInteractiveEnd.mLine > aEnd.mLine)
					break;
				else if (mState.mCursors[c].mInteractiveEnd.mLine != aEnd.mLine)
					continue;
				int otherCursorEndCharIndex = GetCharacterIndexR(mState.mCursors[c].mInteractiveEnd);
				int otherCursorStartCharIndex = GetCharacterIndexR(mState.mCursors[c].mInteractiveStart);
				int otherCursorNewEndCharIndex = GetCharacterIndexR(aStart) + otherCursorEndCharIndex;
				int otherCursorNewStartCharIndex = GetCharacterIndexR(aStart) + otherCursorStartCharIndex;
				auto targetEndCoords = Coordinates(aStart.mLine, GetCharacterColumn(aStart.mLine, otherCursorNewEndCharIndex));
				auto targetStartCoords = Coordinates(aStart.mLine, GetCharacterColumn(aStart.mLine, otherCursorNewStartCharIndex));
				SetCursorPosition(targetStartCoords, c, true);
				SetCursorPosition(targetEndCoords, c, false);
			}
			RemoveLines(aStart.mLine + 1, aEnd.mLine + 1);
		}
	}
}

void TextEditor::DeleteSelection(int aCursor)
{
	if (aCursor == -1)
		aCursor = mState.mCurrentCursor;

	if (mState.mCursors[aCursor].GetSelectionEnd() == mState.mCursors[aCursor].GetSelectionStart())
		return;

	Coordinates newCursorPos = mState.mCursors[aCursor].GetSelectionStart();
	DeleteRange(newCursorPos, mState.mCursors[aCursor].GetSelectionEnd());
	SetCursorPosition(newCursorPos, aCursor);
	Colorize(newCursorPos.mLine, 1);
}

void TextEditor::RemoveGlyphsFromLine(int aLine, int aStartChar, int aEndChar)
{
	int column = GetCharacterColumn(aLine, aStartChar);
	auto& line = mLines[aLine];
	OnLineChanged(true, aLine, column, aEndChar - aStartChar, true);
	line.erase(line.begin() + aStartChar, aEndChar == -1 ? line.end() : line.begin() + aEndChar);
	OnLineChanged(false, aLine, column, aEndChar - aStartChar, true);
}

void TextEditor::AddGlyphsToLine(int aLine, int aTargetIndex, Line::iterator aSourceStart, Line::iterator aSourceEnd)
{
	int targetColumn = GetCharacterColumn(aLine, aTargetIndex);
	int charsInserted = std::distance(aSourceStart, aSourceEnd);
	auto& line = mLines[aLine];
	OnLineChanged(true, aLine, targetColumn, charsInserted, false);
	line.insert(line.begin() + aTargetIndex, aSourceStart, aSourceEnd);
	OnLineChanged(false, aLine, targetColumn, charsInserted, false);
}

void TextEditor::AddGlyphToLine(int aLine, int aTargetIndex, Glyph aGlyph)
{
	int targetColumn = GetCharacterColumn(aLine, aTargetIndex);
	auto& line = mLines[aLine];
	OnLineChanged(true, aLine, targetColumn, 1, false);
	line.insert(line.begin() + aTargetIndex, aGlyph);
	OnLineChanged(false, aLine, targetColumn, 1, false);
}

ImU32 TextEditor::GetGlyphColor(const Glyph& aGlyph) const
{
	if (mLanguageDefinition == nullptr)
		return mPalette[(int)PaletteIndex::Default];
	if (aGlyph.mComment)
		return mPalette[(int)PaletteIndex::Comment];
	if (aGlyph.mMultiLineComment)
		return mPalette[(int)PaletteIndex::MultiLineComment];
	auto const color = mPalette[(int)aGlyph.mColorIndex];
	if (aGlyph.mPreprocessor)
	{
		const auto ppcolor = mPalette[(int)PaletteIndex::Preprocessor];
		const int c0 = ((ppcolor & 0xff) + (color & 0xff)) / 2;
		const int c1 = (((ppcolor >> 8) & 0xff) + ((color >> 8) & 0xff)) / 2;
		const int c2 = (((ppcolor >> 16) & 0xff) + ((color >> 16) & 0xff)) / 2;
		const int c3 = (((ppcolor >> 24) & 0xff) + ((color >> 24) & 0xff)) / 2;
		return ImU32(c0 | (c1 << 8) | (c2 << 16) | (c3 << 24));
	}
	return color;
}

void TextEditor::HandleKeyboardInputs(bool aParentIsFocused)
{
	if (ImGui::IsWindowFocused() || aParentIsFocused)
	{
		if (ImGui::IsWindowHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
		//ImGui::CaptureKeyboardFromApp(true);

		ImGuiIO& io = ImGui::GetIO();
		auto isOSX = io.ConfigMacOSXBehaviors;
		auto alt = io.KeyAlt;
		auto ctrl = io.KeyCtrl;
		auto shift = io.KeyShift;
		auto super = io.KeySuper;

		auto isShortcut = (isOSX ? (super && !ctrl) : (ctrl && !super)) && !alt && !shift;
		auto isShiftShortcut = (isOSX ? (super && !ctrl) : (ctrl && !super)) && shift && !alt;
		auto isWordmoveKey = isOSX ? alt : ctrl;
		auto isAltOnly = alt && !ctrl && !shift && !super;
		auto isCtrlOnly = ctrl && !alt && !shift && !super;
		auto isShiftOnly = shift && !alt && !ctrl && !super;

		// Don't process keyboard input if another ImGui item (like Find panel inputs) is active
		bool shouldProcessInput = !ImGui::IsAnyItemActive();
		
		if (shouldProcessInput)
		{
			io.WantCaptureKeyboard = true;
			io.WantTextInput = true;
		}
		
		// Allow Escape key to close find panel even when its inputs are active
		if (mShowFindPanel && ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			mShowFindPanel = false;
			return;
		}

		// Allow shortcuts to open find panel even when other items are active
		if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_F))
		{
			std::string selection = GetSelectedText();
				if (!selection.empty())
				{
					size_t maxCopy = sizeof(mFindBuffer) - 1;
					std::strncpy(mFindBuffer, selection.c_str(), maxCopy);
					mFindBuffer[maxCopy] = '\0';
					MarkFindResultsDirty(false);
				}
				mShowFindPanel = true;
			mFindFocusRequested = true;
			mReplaceFocusRequested = false;
			EnsureFindResultsUpToDate();
			return;
		}

		if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_H))
		{
			if (!mShowFindPanel)
			{
				mShowFindPanel = true;
				mFindFocusRequested = true;
			}
			mReplaceFocusRequested = true;
			EnsureFindResultsUpToDate();
			return;
		}

		// Only process other keyboard input if no ImGui item is active
		if (!shouldProcessInput)
			return;
		
		// Handle auto-complete navigation
		if (mShowAutoComplete && mLanguageDefinitionId == LanguageDefinitionId::Sql)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				mShowAutoComplete = false;
				mAutoCompleteSuggestions.clear();
				mAutoCompleteSelectedIndex = -1;
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
			{
				if (mAutoCompleteSelectedIndex > 0)
					mAutoCompleteSelectedIndex--;
				return;
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
			{
				if (mAutoCompleteSelectedIndex < (int)mAutoCompleteSuggestions.size() - 1)
					mAutoCompleteSelectedIndex++;
				return;
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_Tab) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
			{
				AcceptAutoComplete();
				return;
			}
		}

		if (ImGui::IsKeyPressed(ImGuiKey_F3))
		{
			FindNext(io.KeyShift);
			return;
		}

		if (!mReadOnly && isShortcut && ImGui::IsKeyPressed(ImGuiKey_Z))
			Undo();
		else if (!mReadOnly && isAltOnly && ImGui::IsKeyPressed(ImGuiKey_Backspace))
			Undo();
		else if (!mReadOnly && isShortcut && ImGui::IsKeyPressed(ImGuiKey_Y))
			Redo();
		else if (!mReadOnly && isShiftShortcut && ImGui::IsKeyPressed(ImGuiKey_Z))
			Redo();
		else if (!alt && !ctrl && !super && ImGui::IsKeyPressed(ImGuiKey_UpArrow))
			MoveUp(1, shift);
		else if (!alt && !ctrl && !super && ImGui::IsKeyPressed(ImGuiKey_DownArrow))
			MoveDown(1, shift);
		else if ((isOSX ? !ctrl : !alt) && !super && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
			MoveLeft(shift, isWordmoveKey);
		else if ((isOSX ? !ctrl : !alt) && !super && ImGui::IsKeyPressed(ImGuiKey_RightArrow))
			MoveRight(shift, isWordmoveKey);
		else if (!alt && !ctrl && !super && ImGui::IsKeyPressed(ImGuiKey_PageUp))
			MoveUp(mVisibleLineCount - 2, shift);
		else if (!alt && !ctrl && !super && ImGui::IsKeyPressed(ImGuiKey_PageDown))
			MoveDown(mVisibleLineCount - 2, shift);
		else if (ctrl && !alt && !super && ImGui::IsKeyPressed(ImGuiKey_Home))
			MoveTop(shift);
		else if (ctrl && !alt && !super && ImGui::IsKeyPressed(ImGuiKey_End))
			MoveBottom(shift);
		else if (!alt && !ctrl && !super && ImGui::IsKeyPressed(ImGuiKey_Home))
			MoveHome(shift);
		else if (!alt && !ctrl && !super && ImGui::IsKeyPressed(ImGuiKey_End))
			MoveEnd(shift);
		else if (!mReadOnly && !alt && !shift && !super && ImGui::IsKeyPressed(ImGuiKey_Delete))
		{
			Delete(ctrl);
			if (mLanguageDefinitionId == LanguageDefinitionId::Sql)
				UpdateAutoComplete();
		}
		else if (!mReadOnly && !alt && !shift && !super && ImGui::IsKeyPressed(ImGuiKey_Backspace))
		{
			Backspace(ctrl);
			if (mLanguageDefinitionId == LanguageDefinitionId::Sql)
				UpdateAutoComplete();
		}
		else if (!mReadOnly && !alt && ctrl && shift && !super && ImGui::IsKeyPressed(ImGuiKey_K))
			RemoveCurrentLines();
		else if (!mReadOnly && !alt && ctrl && !shift && !super && ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
			ChangeCurrentLinesIndentation(false);
		else if (!mReadOnly && !alt && ctrl && !shift && !super && ImGui::IsKeyPressed(ImGuiKey_RightBracket))
			ChangeCurrentLinesIndentation(true);
		else if (!alt && ctrl && shift && !super && ImGui::IsKeyPressed(ImGuiKey_UpArrow))
			MoveUpCurrentLines();
		else if (!alt && ctrl && shift && !super && ImGui::IsKeyPressed(ImGuiKey_DownArrow))
			MoveDownCurrentLines();
		else if (!mReadOnly && !alt && ctrl && !shift && !super && ImGui::IsKeyPressed(ImGuiKey_Slash))
			ToggleLineComment();
		else if (isCtrlOnly && ImGui::IsKeyPressed(ImGuiKey_Insert))
			Copy();
		else if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_C))
			Copy();
		else if (!mReadOnly && isShiftOnly && ImGui::IsKeyPressed(ImGuiKey_Insert))
			Paste();
		else if (!mReadOnly && isShortcut && ImGui::IsKeyPressed(ImGuiKey_V))
			Paste();
		else if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_X))
			Cut();
		else if (isShiftOnly && ImGui::IsKeyPressed(ImGuiKey_Delete))
			Cut();
		else if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_A))
			SelectAll();
		else if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_D))
			AddCursorForNextOccurrence();
        else if (!mReadOnly && !alt && !ctrl && !shift && !super && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
			EnterCharacter('\n', false);
		else if (!mReadOnly && !alt && !ctrl && !super && ImGui::IsKeyPressed(ImGuiKey_Tab))
			EnterCharacter('\t', shift);
		if (!mReadOnly && !io.InputQueueCharacters.empty() && ctrl == alt && !super)
		{
			for (int i = 0; i < io.InputQueueCharacters.Size; i++)
			{
				auto c = io.InputQueueCharacters[i];
				if (c != 0 && (c == '\n' || c >= 32))
				{
					EnterCharacter(c, shift);
					// Update auto-complete after character input for SQL
					if (mLanguageDefinitionId == LanguageDefinitionId::Sql)
						UpdateAutoComplete();
				}
			}
			io.InputQueueCharacters.resize(0);
		}
	}
}

void TextEditor::HandleMouseInputs()
{
	ImGuiIO& io = ImGui::GetIO();
	auto shift = io.KeyShift;
	auto ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
	auto alt = io.ConfigMacOSXBehaviors ? io.KeyCtrl : io.KeyAlt;

	/*
	Pan with middle mouse button
	*/
	mPanning &= ImGui::IsMouseDown(2);
	if (mPanning && ImGui::IsMouseDragging(2))
	{
		ImVec2 scroll = { ImGui::GetScrollX(), ImGui::GetScrollY() };
		ImVec2 currentMousePos = ImGui::GetMouseDragDelta(2);
		ImVec2 mouseDelta = {
			currentMousePos.x - mLastMousePos.x,
			currentMousePos.y - mLastMousePos.y
		};
		ImGui::SetScrollY(scroll.y - mouseDelta.y);
		ImGui::SetScrollX(scroll.x - mouseDelta.x);
		mLastMousePos = currentMousePos;
	}

	// Mouse left button dragging (=> update selection)
	mDraggingSelection &= ImGui::IsMouseDown(0);
	if (mDraggingSelection && ImGui::IsMouseDragging(0))
	{
		io.WantCaptureMouse = true;
		Coordinates cursorCoords = ScreenPosToCoordinates(ImGui::GetMousePos());
		SetCursorPosition(cursorCoords, mState.GetLastAddedCursorIndex(), false);
	}

	if (ImGui::IsWindowHovered())
	{
		auto click = ImGui::IsMouseClicked(0);
		if (!shift && !alt)
		{
			auto doubleClick = ImGui::IsMouseDoubleClicked(0);
			auto t = ImGui::GetTime();
			auto tripleClick = click && !doubleClick &&
				(mLastClickTime != -1.0f && (t - mLastClickTime) < io.MouseDoubleClickTime &&
					Distance(io.MousePos, mLastClickPos) < 0.01f);

			if (click)
				mDraggingSelection = true;

			/*
			Pan with middle mouse button
			*/

			if (ImGui::IsMouseClicked(2))
			{
				mPanning = true;
				mLastMousePos = ImGui::GetMouseDragDelta(2);
			}

			/*
			Left mouse button triple click
			*/

			if (tripleClick)
			{
				if (ctrl)
					mState.AddCursor();
				else
					mState.mCurrentCursor = 0;

				Coordinates cursorCoords = ScreenPosToCoordinates(ImGui::GetMousePos());
				Coordinates targetCursorPos = cursorCoords.mLine < mLines.size() - 1 ?
					Coordinates{ cursorCoords.mLine + 1, 0 } :
					Coordinates{ cursorCoords.mLine, GetLineMaxColumn(cursorCoords.mLine) };
				SetSelection({ cursorCoords.mLine, 0 }, targetCursorPos, mState.mCurrentCursor);

				mLastClickTime = -1.0f;
			}

			/*
			Left mouse button double click
			*/

			else if (doubleClick)
			{
				if (ctrl)
					mState.AddCursor();
				else
					mState.mCurrentCursor = 0;

				Coordinates cursorCoords = ScreenPosToCoordinates(ImGui::GetMousePos());
				SetSelection(FindWordStart(cursorCoords), FindWordEnd(cursorCoords), mState.mCurrentCursor);

				mLastClickTime = (float)ImGui::GetTime();
				mLastClickPos = io.MousePos;
			}

			/*
			Left mouse button click
			*/
			else if (click)
			{
				if (ctrl)
					mState.AddCursor();
				else
					mState.mCurrentCursor = 0;

				bool isOverLineNumber;
				Coordinates cursorCoords = ScreenPosToCoordinates(ImGui::GetMousePos(), &isOverLineNumber);
				if (isOverLineNumber)
				{
					Coordinates targetCursorPos = cursorCoords.mLine < mLines.size() - 1 ?
						Coordinates{ cursorCoords.mLine + 1, 0 } :
						Coordinates{ cursorCoords.mLine, GetLineMaxColumn(cursorCoords.mLine) };
					SetSelection({ cursorCoords.mLine, 0 }, targetCursorPos, mState.mCurrentCursor);
				}
				else
					SetCursorPosition(cursorCoords, mState.GetLastAddedCursorIndex());

				mLastClickTime = (float)ImGui::GetTime();
				mLastClickPos = io.MousePos;
			}
			else if (ImGui::IsMouseReleased(0))
			{
				mState.SortCursorsFromTopToBottom();
				MergeCursorsIfPossible();
			}
		}
		else if (shift)
		{
			if (click)
			{
				Coordinates newSelection = ScreenPosToCoordinates(ImGui::GetMousePos());
				SetCursorPosition(newSelection, mState.mCurrentCursor, false);
			}
		}
	}
}

void TextEditor::UpdateViewVariables(float aScrollX, float aScrollY)
{
	mContentHeight = ImGui::GetWindowHeight() - (IsHorizontalScrollbarVisible() ? IMGUI_SCROLLBAR_WIDTH : 0.0f);
	mContentWidth = ImGui::GetWindowWidth() - (IsVerticalScrollbarVisible() ? IMGUI_SCROLLBAR_WIDTH : 0.0f);

	mVisibleLineCount = Max((int)ceil(mContentHeight / mCharAdvance.y), 0);
	mFirstVisibleLine = Max((int)(aScrollY / mCharAdvance.y), 0);
	mLastVisibleLine = Max((int)((mContentHeight + aScrollY) / mCharAdvance.y), 0);

	mVisibleColumnCount = Max((int)ceil((mContentWidth - Max(mTextStart - aScrollX, 0.0f)) / mCharAdvance.x), 0);
	mFirstVisibleColumn = Max((int)(Max(aScrollX - mTextStart, 0.0f) / mCharAdvance.x), 0);
	mLastVisibleColumn = Max((int)((mContentWidth + aScrollX - mTextStart) / mCharAdvance.x), 0);
}

void TextEditor::Render(bool aParentIsFocused)
{
	ImGuiIO& io = ImGui::GetIO();
	/* Compute mCharAdvance regarding to scaled font size (Ctrl + mouse wheel)*/
	const float fontWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, "#", nullptr, nullptr).x;
	const float fontHeight = ImGui::GetTextLineHeightWithSpacing();
	mCharAdvance = ImVec2(fontWidth, fontHeight * mLineSpacing);

	// Deduce mTextStart by evaluating mLines size (global lineMax) plus two spaces as text width
	mTextStart = mLeftMargin;
	static char lineNumberBuffer[16];
	if (mShowLineNumbers)
	{
		snprintf(lineNumberBuffer, 16, " %zu ", mLines.size());
		mTextStart += ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, lineNumberBuffer, nullptr, nullptr).x;
	}

	ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
	mScrollX = ImGui::GetScrollX();
	mScrollY = ImGui::GetScrollY();
	UpdateViewVariables(mScrollX, mScrollY);
	bool findResultsUpdatedThisFrame = false;
	if (mFindRefreshPending)
	{
		mFindRefreshTimer = std::max(0.0f, mFindRefreshTimer - io.DeltaTime);
		if (mFindRefreshTimer <= 0.0f)
		{
			mFindRefreshPending = false;
			mFindRefreshTimer = 0.0f;
			EnsureFindResultsUpToDate();
			findResultsUpdatedThisFrame = true;
		}
	}

	if (!mFindRefreshPending && !findResultsUpdatedThisFrame)
	{
		mFindRefreshTimer = 0.0f;
		EnsureFindResultsUpToDate();
		findResultsUpdatedThisFrame = true;
	}
	bool drawFindHighlights = HasValidFindPattern() && !mFindResults.empty();
	ImU32 findHighlightColor = 0;
	ImU32 findHighlightActiveColor = 0;
	if (drawFindHighlights)
	{
		ImVec4 baseColor = U32ColorToVec4(mPalette[(int)PaletteIndex::Selection]);
		ImVec4 inactiveColor = baseColor;
		inactiveColor.w *= 0.35f;
		findHighlightColor = ImGui::ColorConvertFloat4ToU32(inactiveColor);
		ImVec4 activeColor = baseColor;
		activeColor.w *= 0.65f;
		findHighlightActiveColor = ImGui::ColorConvertFloat4ToU32(activeColor);
	}

	int maxColumnLimited = 0;
	if (!mLines.empty())
	{
		auto drawList = ImGui::GetWindowDrawList();
		float spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ", nullptr, nullptr).x;

		for (int lineNo = mFirstVisibleLine; lineNo <= mLastVisibleLine && lineNo < mLines.size(); lineNo++)
		{
			ImVec2 lineStartScreenPos = ImVec2(cursorScreenPos.x, cursorScreenPos.y + lineNo * mCharAdvance.y);
			ImVec2 textScreenPos = ImVec2(lineStartScreenPos.x + mTextStart, lineStartScreenPos.y);

			auto& line = mLines[lineNo];
			maxColumnLimited = Max(GetLineMaxColumn(lineNo, mLastVisibleColumn), maxColumnLimited);

			Coordinates lineStartCoord(lineNo, 0);
			Coordinates lineEndCoord(lineNo, maxColumnLimited);

			if (drawFindHighlights)
			{
				const auto* segments = GetFindHighlightsForLine(lineNo);
				if (segments != nullptr)
				{
					for (const auto& segment : *segments)
					{
						Coordinates segmentStart(lineNo, segment.mStartColumn);
						Coordinates segmentEnd(lineNo, segment.mEndColumn);
						float rectStart = TextDistanceToLineStart(segmentStart, false);
						float rectEnd = TextDistanceToLineStart(segmentEnd, false);
						if (segment.mExtendsPastLine)
							rectEnd += mCharAdvance.x;
						bool isActiveSegment = segment.mResultIndex == mFindResultIndex;
						ImU32 color = isActiveSegment ? findHighlightActiveColor : findHighlightColor;
						drawList->AddRectFilled(
							ImVec2{ lineStartScreenPos.x + mTextStart + rectStart, lineStartScreenPos.y },
							ImVec2{ lineStartScreenPos.x + mTextStart + rectEnd, lineStartScreenPos.y + mCharAdvance.y },
							color, 2.5f);
					}
				}
			}

			// Draw selection for the current line
			for (int c = 0; c <= mState.mCurrentCursor; c++)
			{
				float rectStart = -1.0f;
				float rectEnd = -1.0f;
				Coordinates cursorSelectionStart = mState.mCursors[c].GetSelectionStart();
				Coordinates cursorSelectionEnd = mState.mCursors[c].GetSelectionEnd();
				assert(cursorSelectionStart <= cursorSelectionEnd);

				if (cursorSelectionStart <= lineEndCoord)
					rectStart = cursorSelectionStart > lineStartCoord ? TextDistanceToLineStart(cursorSelectionStart) : 0.0f;
				if (cursorSelectionEnd > lineStartCoord)
					rectEnd = TextDistanceToLineStart(cursorSelectionEnd < lineEndCoord ? cursorSelectionEnd : lineEndCoord);
				if (cursorSelectionEnd.mLine > lineNo || cursorSelectionEnd.mLine == lineNo && cursorSelectionEnd > lineEndCoord)
					rectEnd += mCharAdvance.x;

				if (rectStart != -1 && rectEnd != -1 && rectStart < rectEnd)
					drawList->AddRectFilled(
						ImVec2{ lineStartScreenPos.x + mTextStart + rectStart, lineStartScreenPos.y },
						ImVec2{ lineStartScreenPos.x + mTextStart + rectEnd, lineStartScreenPos.y + mCharAdvance.y },
						mPalette[(int)PaletteIndex::Selection]);
			}

			// Draw line number (right aligned)
			if (mShowLineNumbers)
			{
				snprintf(lineNumberBuffer, 16, "%d  ", lineNo + 1);
				float lineNoWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, lineNumberBuffer, nullptr, nullptr).x;
				drawList->AddText(ImVec2(lineStartScreenPos.x + mTextStart - lineNoWidth, lineStartScreenPos.y), mPalette[(int)PaletteIndex::LineNumber], lineNumberBuffer);
			}

			std::vector<Coordinates> cursorCoordsInThisLine;
			for (int c = 0; c <= mState.mCurrentCursor; c++)
			{
				if (mState.mCursors[c].mInteractiveEnd.mLine == lineNo)
					cursorCoordsInThisLine.push_back(mState.mCursors[c].mInteractiveEnd);
			}
			if (cursorCoordsInThisLine.size() > 0)
			{
				bool focused = ImGui::IsWindowFocused() || aParentIsFocused;

				// Render the cursors
				if (focused)
				{
					for (const auto& cursorCoords : cursorCoordsInThisLine)
					{
						float width = 1.0f;
						auto cindex = GetCharacterIndexR(cursorCoords);
						float cx = TextDistanceToLineStart(cursorCoords);

						ImVec2 cstart(textScreenPos.x + cx, lineStartScreenPos.y);
						ImVec2 cend(textScreenPos.x + cx + width, lineStartScreenPos.y + mCharAdvance.y);
						drawList->AddRectFilled(cstart, cend, mPalette[(int)PaletteIndex::Cursor]);
						if (mCursorOnBracket)
						{
							ImVec2 topLeft = { cstart.x, lineStartScreenPos.y + fontHeight + 1.0f };
							ImVec2 bottomRight = { topLeft.x + mCharAdvance.x, topLeft.y + 1.0f };
							drawList->AddRectFilled(topLeft, bottomRight, mPalette[(int)PaletteIndex::Cursor]);
						}
					}
				}
			}

			// Render colorized text
			static std::string glyphBuffer;
			int charIndex = GetFirstVisibleCharacterIndex(lineNo);
			int column = mFirstVisibleColumn; // can be in the middle of tab character
			while (charIndex < mLines[lineNo].size() && column <= mLastVisibleColumn)
			{
				auto& glyph = line[charIndex];
				auto color = GetGlyphColor(glyph);
				ImVec2 targetGlyphPos = { lineStartScreenPos.x + mTextStart + TextDistanceToLineStart({lineNo, column}, false), lineStartScreenPos.y };

				if (glyph.mChar == '\t')
				{
					if (mShowWhitespaces)
					{
						ImVec2 p1, p2, p3, p4;

						const auto s = ImGui::GetFontSize();
						const auto x1 = targetGlyphPos.x + mCharAdvance.x * 0.3f;
						const auto y = targetGlyphPos.y + fontHeight * 0.5f;

						if (mShortTabs)
						{
							const auto x2 = targetGlyphPos.x + mCharAdvance.x;
							p1 = ImVec2(x1, y);
							p2 = ImVec2(x2, y);
							p3 = ImVec2(x2 - s * 0.16f, y - s * 0.16f);
							p4 = ImVec2(x2 - s * 0.16f, y + s * 0.16f);
						}
						else
						{
							const auto x2 = targetGlyphPos.x + TabSizeAtColumn(column) * mCharAdvance.x - mCharAdvance.x * 0.3f;
							p1 = ImVec2(x1, y);
							p2 = ImVec2(x2, y);
							p3 = ImVec2(x2 - s * 0.2f, y - s * 0.2f);
							p4 = ImVec2(x2 - s * 0.2f, y + s * 0.2f);
						}

						drawList->AddLine(p1, p2, mPalette[(int)PaletteIndex::ControlCharacter]);
						drawList->AddLine(p2, p3, mPalette[(int)PaletteIndex::ControlCharacter]);
						drawList->AddLine(p2, p4, mPalette[(int)PaletteIndex::ControlCharacter]);
					}
				}
				else if (glyph.mChar == ' ')
				{
					if (mShowWhitespaces)
					{
						const auto s = ImGui::GetFontSize();
						const auto x = targetGlyphPos.x + spaceSize * 0.5f;
						const auto y = targetGlyphPos.y + s * 0.5f;
						drawList->AddCircleFilled(ImVec2(x, y), 1.5f, mPalette[(int)PaletteIndex::ControlCharacter], 4);
					}
				}
				else
				{
					int seqLength = UTF8CharLength(glyph.mChar);
					if (mCursorOnBracket && seqLength == 1 && mMatchingBracketCoords == Coordinates{ lineNo, column })
					{
						ImVec2 topLeft = { targetGlyphPos.x, targetGlyphPos.y + fontHeight + 1.0f };
						ImVec2 bottomRight = { topLeft.x + mCharAdvance.x, topLeft.y + 1.0f };
						drawList->AddRectFilled(topLeft, bottomRight, mPalette[(int)PaletteIndex::Cursor]);
					}
					glyphBuffer.clear();
					for (int i = 0; i < seqLength; i++)
						glyphBuffer.push_back(line[charIndex + i].mChar);
					drawList->AddText(targetGlyphPos, color, glyphBuffer.c_str());
				}

				MoveCharIndexAndColumn(lineNo, charIndex, column);
			}
		}
	}
	mCurrentSpaceHeight = (mLines.size() + Min(mVisibleLineCount - 1, (int)mLines.size())) * mCharAdvance.y;
	mCurrentSpaceWidth = Max((maxColumnLimited + Min(mVisibleColumnCount - 1, maxColumnLimited)) * mCharAdvance.x, mCurrentSpaceWidth);

	ImGui::SetCursorPos(ImVec2(0, 0));
	ImGui::Dummy(ImVec2(mCurrentSpaceWidth, mCurrentSpaceHeight));

	if (mEnsureCursorVisible > -1)
	{
		for (int i = 0; i < (mEnsureCursorVisibleStartToo ? 2 : 1); i++) // first pass for interactive end and second pass for interactive start
		{
			if (i) UpdateViewVariables(mScrollX, mScrollY); // second pass depends on changes made in first pass
			Coordinates targetCoords = GetSanitizedCursorCoordinates(mEnsureCursorVisible, i); // cursor selection end or start
			if (targetCoords.mLine <= mFirstVisibleLine)
			{
				float targetScroll = std::max(0.0f, (targetCoords.mLine - 0.5f) * mCharAdvance.y);
				if (targetScroll < mScrollY)
					ImGui::SetScrollY(targetScroll);
			}
			if (targetCoords.mLine >= mLastVisibleLine)
			{
				float targetScroll = std::max(0.0f, (targetCoords.mLine + 1.5f) * mCharAdvance.y - mContentHeight);
				if (targetScroll > mScrollY)
					ImGui::SetScrollY(targetScroll);
			}
			if (targetCoords.mColumn <= mFirstVisibleColumn)
			{
				float targetScroll = std::max(0.0f, mTextStart + (targetCoords.mColumn - 0.5f) * mCharAdvance.x);
				if (targetScroll < mScrollX)
					ImGui::SetScrollX(mScrollX = targetScroll);
			}
			if (targetCoords.mColumn >= mLastVisibleColumn)
			{
				float targetScroll = std::max(0.0f, mTextStart + (targetCoords.mColumn + 0.5f) * mCharAdvance.x - mContentWidth);
				if (targetScroll > mScrollX)
					ImGui::SetScrollX(mScrollX = targetScroll);
			}
		}
		mEnsureCursorVisible = -1;
	}
	if (mScrollToTop)
	{
		ImGui::SetScrollY(0.0f);
		mScrollToTop = false;
	}
	if (mSetViewAtLine > -1)
	{
		float targetScroll;
		switch (mSetViewAtLineMode)
		{
		default:
		case SetViewAtLineMode::FirstVisibleLine:
			targetScroll = std::max(0.0f, (float)mSetViewAtLine * mCharAdvance.y);
			break;
		case SetViewAtLineMode::LastVisibleLine:
			targetScroll = std::max(0.0f, (float)(mSetViewAtLine - (mLastVisibleLine - mFirstVisibleLine)) * mCharAdvance.y);
			break;
		case SetViewAtLineMode::Centered:
			targetScroll = std::max(0.0f, ((float)mSetViewAtLine - (float)(mLastVisibleLine - mFirstVisibleLine) * 0.5f) * mCharAdvance.y);
			break;
		}
		ImGui::SetScrollY(targetScroll);
		mSetViewAtLine = -1;
	}
}

void TextEditor::OnCursorPositionChanged()
{
	if (mState.mCurrentCursor == 0 && !mState.mCursors[0].HasSelection()) // only one cursor without selection
		mCursorOnBracket = FindMatchingBracket(mState.mCursors[0].mInteractiveEnd.mLine,
			GetCharacterIndexR(mState.mCursors[0].mInteractiveEnd), mMatchingBracketCoords);
	else
		mCursorOnBracket = false;

	if (!mDraggingSelection)
	{
		mState.SortCursorsFromTopToBottom();
		MergeCursorsIfPossible();
	}
}

void TextEditor::OnLineChanged(bool aBeforeChange, int aLine, int aColumn, int aCharCount, bool aDeleted) // adjusts cursor position when other cursor writes/deletes in the same line
{
	static std::unordered_map<int, int> cursorCharIndices;
	if (aBeforeChange)
	{
		cursorCharIndices.clear();
		for (int c = 0; c <= mState.mCurrentCursor; c++)
		{
			if (mState.mCursors[c].mInteractiveEnd.mLine == aLine && // cursor is at the line
				mState.mCursors[c].mInteractiveEnd.mColumn > aColumn && // cursor is to the right of changing part
				mState.mCursors[c].GetSelectionEnd() == mState.mCursors[c].GetSelectionStart()) // cursor does not have a selection
			{
				cursorCharIndices[c] = GetCharacterIndexR({ aLine, mState.mCursors[c].mInteractiveEnd.mColumn });
				cursorCharIndices[c] += aDeleted ? -aCharCount : aCharCount;
			}
		}
	}
	else
	{
		for (auto& item : cursorCharIndices)
			SetCursorPosition({ aLine, GetCharacterColumn(aLine, item.second) }, item.first);
	}
}

void TextEditor::MergeCursorsIfPossible()
{
	// requires the cursors to be sorted from top to bottom
	std::unordered_set<int> cursorsToDelete;
	if (AnyCursorHasSelection())
	{
		// merge cursors if they overlap
		for (int c = mState.mCurrentCursor; c > 0; c--)// iterate backwards through pairs
		{
			int pc = c - 1; // pc for previous cursor

			bool pcContainsC = mState.mCursors[pc].GetSelectionEnd() >= mState.mCursors[c].GetSelectionEnd();
			bool pcContainsStartOfC = mState.mCursors[pc].GetSelectionEnd() > mState.mCursors[c].GetSelectionStart();

			if (pcContainsC)
			{
				cursorsToDelete.insert(c);
			}
			else if (pcContainsStartOfC)
			{
				Coordinates pcStart = mState.mCursors[pc].GetSelectionStart();
				Coordinates cEnd = mState.mCursors[c].GetSelectionEnd();
				mState.mCursors[pc].mInteractiveEnd = cEnd;
				mState.mCursors[pc].mInteractiveStart = pcStart;
				cursorsToDelete.insert(c);
			}
		}
	}
	else
	{
		// merge cursors if they are at the same position
		for (int c = mState.mCurrentCursor; c > 0; c--)// iterate backwards through pairs
		{
			int pc = c - 1;
			if (mState.mCursors[pc].mInteractiveEnd == mState.mCursors[c].mInteractiveEnd)
				cursorsToDelete.insert(c);
		}
	}
	for (int c = mState.mCurrentCursor; c > -1; c--)// iterate backwards through each of them
	{
		if (cursorsToDelete.find(c) != cursorsToDelete.end())
			mState.mCursors.erase(mState.mCursors.begin() + c);
	}
	mState.mCurrentCursor -= cursorsToDelete.size();
}

void TextEditor::AddUndo(UndoRecord& aValue)
{
	assert(!mReadOnly);
	mUndoBuffer.resize((size_t)(mUndoIndex + 1));
	mUndoBuffer.back() = aValue;
	++mUndoIndex;
}

bool TextEditor::HasValidFindPattern() const
{
	return mFindBuffer[0] != '\0';
}

const std::vector<TextEditor::LineHighlight>* TextEditor::GetFindHighlightsForLine(int aLineNumber) const
{
	if (!HasValidFindPattern())
		return nullptr;
	auto it = mFindHighlightsCache.find(aLineNumber);
	return it != mFindHighlightsCache.end() ? &it->second : nullptr;
}

TextEditor::Coordinates TextEditor::AdvanceCoordinates(const Coordinates& aCoords) const
{
	Coordinates sanitized = SanitizeCoordinates(aCoords);
	int totalLines = static_cast<int>(mLines.size());
	if (sanitized.mLine < 0 || sanitized.mLine >= totalLines)
		return Coordinates(totalLines, 0);

	int line = sanitized.mLine;
	int charIndex = GetCharacterIndexR(sanitized);
	if (charIndex < 0)
		charIndex = 0;

	if (!Move(line, charIndex))
	{
		// Move failed only when we're at the logical end of the buffer; return a sentinel past-the-end position
		return Coordinates(totalLines, 0);
	}

	return Coordinates(line, GetCharacterColumn(line, charIndex));
}

bool TextEditor::IsWholeWordMatch(const Coordinates& aStart, const Coordinates& aEnd) const
{
	Coordinates start = SanitizeCoordinates(aStart);
	Coordinates end = SanitizeCoordinates(aEnd);
	int startLine = start.mLine;
	if (startLine < 0 || startLine >= (int)mLines.size())
		return false;
	int startCharIndex = GetCharacterIndexR(start);
	bool boundaryBefore = true;
	if (startCharIndex > 0)
	{
		int prevIndex = startCharIndex - 1;
		while (prevIndex > 0 && IsUTFSequence(mLines[startLine][prevIndex].mChar))
			--prevIndex;
		if (prevIndex >= 0 && prevIndex < (int)mLines[startLine].size())
		{
			char prevChar = mLines[startLine][prevIndex].mChar;
			boundaryBefore = !CharIsWordChar(prevChar);
		}
	}

	int endLine = end.mLine;
	if (endLine < 0 || endLine >= (int)mLines.size())
		return false;
	int endCharIndex = GetCharacterIndexR(end);
	bool boundaryAfter = true;
	if (endCharIndex < (int)mLines[endLine].size())
	{
		char nextChar = mLines[endLine][endCharIndex].mChar;
		boundaryAfter = !CharIsWordChar(nextChar);
	}

	return boundaryBefore && boundaryAfter;
}

void TextEditor::EnsureFindResultsUpToDate()
{
	if (!HasValidFindPattern())
	{
		if (!mFindResults.empty())
		{
			mFindResults.clear();
			mFindHighlightsCache.clear();
			mFindResultIndex = -1;
		}
		mFindResultsDirty = false;
		return;
	}

	int undoSize = (int)mUndoBuffer.size();
	if (mFindResultsDirty || mFindLastUndoIndex != mUndoIndex || mFindLastUndoBufferSize != undoSize)
		RefreshFindResults();
}

void TextEditor::RefreshFindResults(bool aPreserveSelection)
{
	mFindResultsDirty = false;
	mFindRefreshPending = false;
	mFindRefreshTimer = 0.0f;
	mFindLastUndoIndex = mUndoIndex;
	mFindLastUndoBufferSize = (int)mUndoBuffer.size();
	mFindResults.clear();
	mFindHighlightsCache.clear();
	mFindResultIndex = -1;

	if (!HasValidFindPattern() || mLines.empty())
		return;

	std::string pattern(mFindBuffer);
	if (pattern.empty())
		return;

	bool caseSensitive = mFindCaseSensitive;
	bool wholeWord = mFindWholeWord && !mFindUseRegex;
	bool useRegex = mFindUseRegex;

	std::vector<std::string> lineStrings;
	std::vector<size_t> lineOffsets;
	lineStrings.reserve(mLines.size());
	lineOffsets.reserve(mLines.size());

	size_t totalLength = 0;
	for (size_t i = 0; i < mLines.size(); ++i)
	{
		lineOffsets.push_back(totalLength);
		const auto& line = mLines[i];
		std::string lineText;
		lineText.reserve(line.size());
		for (const auto& glyph : line)
			lineText.push_back(glyph.mChar);
		totalLength += lineText.size();
		if (i + 1 < mLines.size())
			totalLength += 1;
		lineStrings.emplace_back(std::move(lineText));
	}

	std::string joined;
	joined.reserve(totalLength);
	for (size_t i = 0; i < lineStrings.size(); ++i)
	{
		joined.append(lineStrings[i]);
		if (i + 1 < lineStrings.size())
			joined.push_back('\n');
	}

	auto coordinateToOffset = [&](const Coordinates& coords) -> size_t
	{
		Coordinates sanitized = SanitizeCoordinates(coords);
		int line = std::clamp(sanitized.mLine, 0, (int)mLines.size() - 1);
		sanitized.mLine = line;
		int charIndex = GetCharacterIndexR(sanitized);
		charIndex = std::clamp(charIndex, 0, (int)mLines[line].size());
		size_t base = lineOffsets[line];
		return base + static_cast<size_t>(charIndex);
	};

	auto offsetToCoordinates = [&](size_t offset) -> Coordinates
	{
		if (lineOffsets.empty())
			return Coordinates(0, 0);
		if (offset > joined.size())
			offset = joined.size();

		auto it = std::upper_bound(lineOffsets.begin(), lineOffsets.end(), offset);
		int line = (int)std::distance(lineOffsets.begin(), it) - 1;
		if (line < 0)
			line = 0;
		if (line >= (int)lineOffsets.size())
			line = (int)lineOffsets.size() - 1;

		size_t lineOffset = lineOffsets[line];
		size_t charIndex = offset - lineOffset;
		if (charIndex > lineStrings[line].size())
			charIndex = lineStrings[line].size();
		int column = GetCharacterColumn(line, (int)charIndex);
		return Coordinates(line, column);
	};

	Coordinates selectionStartCoords;
	Coordinates selectionEndCoords;
	bool selectionRangeValid = false;
	if (mFindSelectionOnly)
	{
		if (TryGetSelectionBounds(selectionStartCoords, selectionEndCoords))
		{
			selectionRangeValid = true;
		}
		else if (mFindSelectionRangeValid)
		{
			selectionRangeValid = true;
			selectionStartCoords = mFindSelectionRangeStart;
			selectionEndCoords = mFindSelectionRangeEnd;
		}
		if (selectionRangeValid)
		{
			selectionStartCoords = SanitizeCoordinates(selectionStartCoords);
			selectionEndCoords = SanitizeCoordinates(selectionEndCoords);
			mFindSelectionRangeStart = selectionStartCoords;
			mFindSelectionRangeEnd = selectionEndCoords;
		}
	}

	mFindSelectionRangeValid = selectionRangeValid;
	if (!selectionRangeValid)
	{
		selectionStartCoords = Coordinates(0, 0);
		selectionEndCoords = Coordinates((int)mLines.size() - 1, GetLineMaxColumn((int)mLines.size() - 1));
		selectionStartCoords = SanitizeCoordinates(selectionStartCoords);
		selectionEndCoords = SanitizeCoordinates(selectionEndCoords);
	}

	size_t rangeStartOffset = coordinateToOffset(selectionStartCoords);
	size_t rangeEndOffset = coordinateToOffset(selectionEndCoords);
	rangeEndOffset = std::min(rangeEndOffset, joined.size());
	if (rangeStartOffset > rangeEndOffset)
		std::swap(rangeStartOffset, rangeEndOffset);

	Coordinates preservedSelectionStart;
	Coordinates preservedSelectionEnd;
	bool preservedSelectionValid = false;
	if (aPreserveSelection && AnyCursorHasSelection())
	{
		int cursorIndex = mState.GetLastAddedCursorIndex();
		preservedSelectionStart = mState.mCursors[cursorIndex].GetSelectionStart();
		preservedSelectionEnd = mState.mCursors[cursorIndex].GetSelectionEnd();
		preservedSelectionValid = true;
	}

	auto addResult = [&](size_t startOffset, size_t endOffset)
	{
		if (startOffset >= endOffset)
			return;
		Coordinates startCoord = offsetToCoordinates(startOffset);
		Coordinates endCoord = offsetToCoordinates(endOffset);
		SearchResult result{ startCoord, endCoord };
		mFindResults.push_back(result);
		int resultIndex = (int)mFindResults.size() - 1;
		int startLine = result.mStart.mLine;
		int endLine = result.mEnd.mLine;
		if (startLine == endLine)
		{
			mFindHighlightsCache[startLine].push_back({ result.mStart.mColumn, result.mEnd.mColumn, false, resultIndex });
		}
		else
		{
			mFindHighlightsCache[startLine].push_back({ result.mStart.mColumn, GetLineMaxColumn(startLine), true, resultIndex });
			for (int line = startLine + 1; line < endLine; ++line)
			{
				mFindHighlightsCache[line].push_back({ 0, GetLineMaxColumn(line), true, resultIndex });
			}
			mFindHighlightsCache[endLine].push_back({ 0, result.mEnd.mColumn, false, resultIndex });
		}
	};

	if (useRegex)
	{
		try
		{
			std::regex_constants::syntax_option_type options = std::regex_constants::ECMAScript | std::regex_constants::optimize;
			if (!caseSensitive)
				options |= std::regex_constants::icase;
			std::regex re(pattern, options);

			auto beginIt = joined.begin() + (std::ptrdiff_t)rangeStartOffset;
			auto endIt = joined.begin() + (std::ptrdiff_t)rangeEndOffset;

			for (auto it = std::sregex_iterator(beginIt, endIt, re); it != std::sregex_iterator(); ++it)
			{
				size_t relativeStart = static_cast<size_t>(it->position());
				size_t matchLength = static_cast<size_t>(it->length());
				if (matchLength == 0)
					continue;

				size_t matchStart = rangeStartOffset + relativeStart;
				size_t matchEnd = matchStart + matchLength;
				if (wholeWord)
				{
					bool boundaryBefore = (matchStart == rangeStartOffset) || (matchStart == 0) || !CharIsWordChar(joined[matchStart - 1]);
					bool boundaryAfter = (matchEnd >= rangeEndOffset) || (matchEnd >= joined.size()) || !CharIsWordChar(joined[matchEnd]);
					if (!boundaryBefore || !boundaryAfter)
						continue;
				}
				addResult(matchStart, matchEnd);
			}
		}
		catch (const std::regex_error&)
		{
			mFindStatusMessage = "Invalid regex";
			mFindStatusTimer = 3.0f;
			return;
		}
	}
	else
	{
		const std::string* haystackPtr = &joined;
		std::string loweredHaystack;
		std::string loweredPattern;
		const std::string* patternPtr = &pattern;
		if (!caseSensitive)
		{
			loweredHaystack = joined;
			std::transform(loweredHaystack.begin(), loweredHaystack.end(), loweredHaystack.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			loweredPattern = pattern;
			std::transform(loweredPattern.begin(), loweredPattern.end(), loweredPattern.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			haystackPtr = &loweredHaystack;
			patternPtr = &loweredPattern;
		}

		size_t patternLength = pattern.size();
		if (patternLength == 0)
			return;

		size_t searchPos = rangeStartOffset;
		while (searchPos < rangeEndOffset)
		{
			size_t found = haystackPtr->find(*patternPtr, searchPos);
			if (found == std::string::npos || found >= rangeEndOffset)
				break;

			size_t matchStart = found;
			size_t matchEnd = matchStart + patternLength;

			if (matchEnd > rangeEndOffset)
				break;

			if (wholeWord)
			{
				bool boundaryBefore = (matchStart == rangeStartOffset) || (matchStart == 0) || !CharIsWordChar(joined[matchStart - 1]);
				bool boundaryAfter = (matchEnd >= rangeEndOffset) || (matchEnd >= joined.size()) || !CharIsWordChar(joined[matchEnd]);
				if (!boundaryBefore || !boundaryAfter)
				{
					searchPos = matchStart + 1;
					continue;
				}
			}

			addResult(matchStart, matchEnd);
			if (matchEnd <= matchStart)
			{
				// safety net against zero-length advances
				searchPos = matchStart + 1;
			}
			else
				searchPos = matchEnd;
		}
	}

	if (mFindResults.empty())
		return;

	Coordinates cursorCoords = GetSanitizedCursorCoordinates();
	size_t cursorOffset = coordinateToOffset(cursorCoords);
	int chosenIndex = -1;

	if (aPreserveSelection && preservedSelectionValid)
	{
		size_t preservedStart = coordinateToOffset(preservedSelectionStart);
		size_t preservedEnd = coordinateToOffset(preservedSelectionEnd);
		for (int i = 0; i < (int)mFindResults.size(); ++i)
		{
			const auto& res = mFindResults[i];
			size_t resStart = coordinateToOffset(res.mStart);
			size_t resEnd = coordinateToOffset(res.mEnd);
			if (resStart == preservedStart && resEnd == preservedEnd)
			{
				chosenIndex = i;
				break;
			}
		}
	}

	if (chosenIndex == -1)
	{
		for (int i = 0; i < (int)mFindResults.size(); ++i)
		{
			const auto& res = mFindResults[i];
			size_t resStart = coordinateToOffset(res.mStart);
			size_t resEnd = coordinateToOffset(res.mEnd);
			if (resStart <= cursorOffset && cursorOffset < resEnd)
			{
				chosenIndex = i;
				break;
			}
			if (cursorOffset < resStart)
			{
				chosenIndex = i;
				break;
			}
		}
	}

	if (chosenIndex == -1)
		chosenIndex = 0;

	mFindResultIndex = chosenIndex;
}

bool TextEditor::FocusFindResult(int aIndex, bool aCenterView)
{
	EnsureFindResultsUpToDate();
	if (mFindResults.empty())
		return false;
	int count = (int)mFindResults.size();
	int idx = aIndex;
	if (idx < 0)
	{
		idx = (idx % count + count) % count;
	}
	else
		idx = idx % count;
	mFindResultIndex = idx;

	const auto& res = mFindResults[mFindResultIndex];
	ClearSelections();
	ClearExtraCursors();
	SetSelection(res.mStart, res.mEnd);
	if (aCenterView)
		EnsureCursorVisible(-1, true);
	return true;
}

void TextEditor::FindNext(bool aBackwards)
{
	EnsureFindResultsUpToDate();
	if (mFindResults.empty())
	{
		mFindStatusMessage = "No matches";
		mFindStatusTimer = 2.5f;
		return;
	}
	int count = (int)mFindResults.size();
	int idx = mFindResultIndex;
	if (idx < 0)
	{
		idx = aBackwards ? count - 1 : 0;
	}
	else
	{
		int next = aBackwards ? idx - 1 : idx + 1;
		if (!mFindWrapAround && (next < 0 || next >= count))
		{
			mFindStatusMessage = aBackwards ? "Reached start" : "Reached end";
			mFindStatusTimer = 2.0f;
			return;
		}
		idx = (next % count + count) % count;
	}
	FocusFindResult(idx);
	mFindStatusMessage.clear();
	mFindStatusTimer = 0.0f;
}

void TextEditor::ReplaceCurrent()
{
	if (!HasValidFindPattern())
	{
		mFindStatusMessage = "Nothing to replace";
		mFindStatusTimer = 2.5f;
		return;
	}

	EnsureFindResultsUpToDate();
	if (mFindResults.empty())
	{
		mFindStatusMessage = "No matches";
		mFindStatusTimer = 2.5f;
		return;
	}
	if (mFindResultIndex < 0 || mFindResultIndex >= (int)mFindResults.size())
		mFindResultIndex = 0;

	const auto current = mFindResults[mFindResultIndex];
	ClearSelections();
	ClearExtraCursors();
	SetSelection(current.mStart, current.mEnd);
	InsertTextAtCursor(mReplaceBuffer);
	if (mFindSelectionOnly)
		mFindSelectionRangeValid = false;

	MarkFindResultsDirty(false);
	RefreshFindResults(false);
	if (!mFindResults.empty())
	{
		Coordinates cursor = GetSanitizedCursorCoordinates();
		int nextIndex = -1;
		for (int i = 0; i < (int)mFindResults.size(); ++i)
		{
			const auto& res = mFindResults[i];
			if (!(cursor < res.mStart) && cursor < res.mEnd)
			{
				nextIndex = i;
				break;
			}
			if (!(res.mStart < cursor))
			{
				nextIndex = i;
				break;
			}
		}
		if (nextIndex == -1)
			nextIndex = 0;
		FocusFindResult(nextIndex);
	}
	else
	{
		mFindResultIndex = -1;
		ClearSelections();
		EnsureCursorVisible();
	}

	mFindStatusMessage = "Replaced";
	mFindStatusTimer = 2.0f;
}

int TextEditor::ReplaceAll()
{
	if (!HasValidFindPattern())
	{
		mFindStatusMessage = "Nothing to replace";
		mFindStatusTimer = 2.5f;
		return 0;
	}

	EnsureFindResultsUpToDate();
	if (mFindResults.empty())
	{
		mFindStatusMessage = "No matches";
		mFindStatusTimer = 2.5f;
		return 0;
	}

	Coordinates selectionStart;
	Coordinates selectionEnd;
	bool selectionRangeActive = false;
	if (mFindSelectionOnly)
	{
		if (TryGetSelectionBounds(selectionStart, selectionEnd))
		{
			mFindSelectionRangeStart = selectionStart;
			mFindSelectionRangeEnd = selectionEnd;
			mFindSelectionRangeValid = true;
		}
		if (mFindSelectionRangeValid)
		{
			selectionStart = SanitizeCoordinates(mFindSelectionRangeStart);
			selectionEnd = SanitizeCoordinates(mFindSelectionRangeEnd);
			selectionRangeActive = true;
		}
	}

	auto withinSelectionRange = [&](const SearchResult& res)
	{
		if (!selectionRangeActive)
			return true;
		return !(res.mStart < selectionStart) && !(selectionEnd < res.mEnd);
	};

	int replacements = 0;
	Coordinates lastReplacementStart = Coordinates::Invalid();

	while (true)
	{
		EnsureFindResultsUpToDate();
		if (mFindResults.empty())
			break;

		int targetIndex = -1;
		for (int i = 0; i < (int)mFindResults.size(); ++i)
		{
			if (withinSelectionRange(mFindResults[i]))
			{
				targetIndex = i;
				break;
			}
		}
		if (targetIndex == -1)
			break;

		const auto current = mFindResults[targetIndex];
		if (lastReplacementStart == current.mStart)
			break;
		lastReplacementStart = current.mStart;

		ClearSelections();
		ClearExtraCursors();
		SetSelection(current.mStart, current.mEnd);
		InsertTextAtCursor(mReplaceBuffer);
		++replacements;
		if (selectionRangeActive)
			mFindSelectionRangeValid = false;
	}

	RefreshFindResults(false);
	if (!mFindResults.empty())
		FocusFindResult(0, false);
	else
	{
		mFindResultIndex = -1;
		ClearSelections();
		EnsureCursorVisible();
	}

	if (replacements == 0)
	{
		mFindStatusMessage = "No matches";
		mFindStatusTimer = 2.5f;
		return 0;
	}

	mFindStatusMessage = replacements == 1 ? "Replaced 1 match" : "Replaced " + std::to_string(replacements) + " matches";
	mFindStatusTimer = 3.0f;
	return replacements;
}

// TODO
// - multiline comments vs single-line: latter is blocking start of a ML
void TextEditor::Colorize(int aFromLine, int aLines)
{
	int toLine = aLines == -1 ? (int)mLines.size() : std::min((int)mLines.size(), aFromLine + aLines);
	mColorRangeMin = std::min(mColorRangeMin, aFromLine);
	mColorRangeMax = std::max(mColorRangeMax, toLine);
	mColorRangeMin = std::max(0, mColorRangeMin);
	mColorRangeMax = std::max(mColorRangeMin, mColorRangeMax);
	mCheckComments = true;
}

void TextEditor::ColorizeRange(int aFromLine, int aToLine)
{
	if (mLines.empty() || aFromLine >= aToLine || mLanguageDefinition == nullptr)
		return;

	std::string buffer;
	boost::cmatch results;
	std::string id;

	int endLine = std::max(0, std::min((int)mLines.size(), aToLine));
	for (int i = aFromLine; i < endLine; ++i)
	{
		auto& line = mLines[i];

		if (line.empty())
			continue;

		buffer.resize(line.size());
		for (size_t j = 0; j < line.size(); ++j)
		{
			auto& col = line[j];
			buffer[j] = col.mChar;
			col.mColorIndex = PaletteIndex::Default;
		}

		const char* bufferBegin = &buffer.front();
		const char* bufferEnd = bufferBegin + buffer.size();

		auto last = bufferEnd;

		for (auto first = bufferBegin; first != last; )
		{
			const char* token_begin = nullptr;
			const char* token_end = nullptr;
			PaletteIndex token_color = PaletteIndex::Default;

			bool hasTokenizeResult = false;

			if (mLanguageDefinition->mTokenize != nullptr)
			{
				if (mLanguageDefinition->mTokenize(first, last, token_begin, token_end, token_color))
					hasTokenizeResult = true;
			}

			if (hasTokenizeResult == false)
			{
				// todo : remove
				//printf("using regex for %.*s\n", first + 10 < last ? 10 : int(last - first), first);

				for (const auto& p : mRegexList->mValue)
				{
					bool regexSearchResult = false;
					try { regexSearchResult = boost::regex_search(first, last, results, p.first, boost::regex_constants::match_continuous); }
					catch (...) {}
					if (regexSearchResult)
					{
						hasTokenizeResult = true;

						auto& v = *results.begin();
						token_begin = v.first;
						token_end = v.second;
						token_color = p.second;
						break;
					}
				}
			}

			if (hasTokenizeResult == false)
			{
				first++;
			}
			else
			{
				const size_t token_length = token_end - token_begin;

				if (token_color == PaletteIndex::Identifier)
				{
					id.assign(token_begin, token_end);

					// todo : allmost all language definitions use lower case to specify keywords, so shouldn't this use ::tolower ?
					if (!mLanguageDefinition->mCaseSensitive)
						std::transform(id.begin(), id.end(), id.begin(), ::toupper);

					if (!line[first - bufferBegin].mPreprocessor)
					{
						if (mLanguageDefinition->mKeywords.count(id) != 0)
							token_color = PaletteIndex::Keyword;
						else if (mLanguageDefinition->mIdentifiers.count(id) != 0)
							token_color = PaletteIndex::KnownIdentifier;
						else if (mLanguageDefinition->mPreprocIdentifiers.count(id) != 0)
							token_color = PaletteIndex::PreprocIdentifier;
					}
					else
					{
						if (mLanguageDefinition->mPreprocIdentifiers.count(id) != 0)
							token_color = PaletteIndex::PreprocIdentifier;
					}
				}

				for (size_t j = 0; j < token_length; ++j)
					line[(token_begin - bufferBegin) + j].mColorIndex = token_color;

				first = token_end;
			}
		}
	}
}

template<class InputIt1, class InputIt2, class BinaryPredicate>
bool ColorizerEquals(InputIt1 first1, InputIt1 last1,
	InputIt2 first2, InputIt2 last2, BinaryPredicate p)
{
	for (; first1 != last1 && first2 != last2; ++first1, ++first2)
	{
		if (!p(*first1, *first2))
			return false;
	}
	return first1 == last1 && first2 == last2;
}
void TextEditor::ColorizeInternal()
{
	if (mLines.empty() || mLanguageDefinition == nullptr)
		return;

	if (mCheckComments)
	{
		auto endLine = mLines.size();
		auto endIndex = 0;
		auto commentStartLine = endLine;
		auto commentStartIndex = endIndex;
		auto withinString = false;
		auto withinSingleLineComment = false;
		auto withinPreproc = false;
		auto firstChar = true;			// there is no other non-whitespace characters in the line before
		auto concatenate = false;		// '\' on the very end of the line
		auto currentLine = 0;
		auto currentIndex = 0;
		while (currentLine < endLine || currentIndex < endIndex)
		{
			auto& line = mLines[currentLine];

			if (currentIndex == 0 && !concatenate)
			{
				withinSingleLineComment = false;
				withinPreproc = false;
				firstChar = true;
			}

			concatenate = false;

			if (!line.empty())
			{
				auto& g = line[currentIndex];
				auto c = g.mChar;

				if (c != mLanguageDefinition->mPreprocChar && !isspace(c))
					firstChar = false;

				if (currentIndex == (int)line.size() - 1 && line[line.size() - 1].mChar == '\\')
					concatenate = true;

				bool inComment = (commentStartLine < currentLine || (commentStartLine == currentLine && commentStartIndex <= currentIndex));

				if (withinString)
				{
					line[currentIndex].mMultiLineComment = inComment;

					if (c == '\"')
					{
						if (currentIndex + 1 < (int)line.size() && line[currentIndex + 1].mChar == '\"')
						{
							currentIndex += 1;
							if (currentIndex < (int)line.size())
								line[currentIndex].mMultiLineComment = inComment;
						}
						else
							withinString = false;
					}
					else if (c == '\\')
					{
						currentIndex += 1;
						if (currentIndex < (int)line.size())
							line[currentIndex].mMultiLineComment = inComment;
					}
				}
				else
				{
					if (firstChar && c == mLanguageDefinition->mPreprocChar)
						withinPreproc = true;

					if (c == '\"')
					{
						withinString = true;
						line[currentIndex].mMultiLineComment = inComment;
					}
					else
					{
						auto pred = [](const char& a, const Glyph& b) { return a == b.mChar; };
						auto from = line.begin() + currentIndex;
						auto& startStr = mLanguageDefinition->mCommentStart;
						auto& singleStartStr = mLanguageDefinition->mSingleLineComment;

						if (!withinSingleLineComment && currentIndex + startStr.size() <= line.size() &&
							ColorizerEquals(startStr.begin(), startStr.end(), from, from + startStr.size(), pred))
						{
							commentStartLine = currentLine;
							commentStartIndex = currentIndex;
						}
						else if (singleStartStr.size() > 0 &&
							currentIndex + singleStartStr.size() <= line.size() &&
							ColorizerEquals(singleStartStr.begin(), singleStartStr.end(), from, from + singleStartStr.size(), pred))
						{
							withinSingleLineComment = true;
						}

						inComment = (commentStartLine < currentLine || (commentStartLine == currentLine && commentStartIndex <= currentIndex));

						line[currentIndex].mMultiLineComment = inComment;
						line[currentIndex].mComment = withinSingleLineComment;

						auto& endStr = mLanguageDefinition->mCommentEnd;
						if (currentIndex + 1 >= (int)endStr.size() &&
							ColorizerEquals(endStr.begin(), endStr.end(), from + 1 - endStr.size(), from + 1, pred))
						{
							commentStartIndex = endIndex;
							commentStartLine = endLine;
						}
					}
				}
				if (currentIndex < (int)line.size())
					line[currentIndex].mPreprocessor = withinPreproc;
				currentIndex += UTF8CharLength(c);
				if (currentIndex >= (int)line.size())
				{
					currentIndex = 0;
					++currentLine;
				}
			}
			else
			{
				currentIndex = 0;
				++currentLine;
			}
		}
		mCheckComments = false;
	}

	if (mColorRangeMin < mColorRangeMax)
	{
		const int increment = (mLanguageDefinition->mTokenize == nullptr) ? 10 : 10000;
		const int to = std::min(mColorRangeMin + increment, mColorRangeMax);
		ColorizeRange(mColorRangeMin, to);
		mColorRangeMin = to;

		if (mColorRangeMax == mColorRangeMin)
		{
			mColorRangeMin = std::numeric_limits<int>::max();
			mColorRangeMax = 0;
		}
		return;
	}
}

const TextEditor::Palette& TextEditor::GetDarkPalette()
{
	const static Palette p = { {
			0xdcdfe4ff,	// Default
			0xe06c75ff,	// Keyword
			0xe5c07bff,	// Number
			0x98c379ff,	// String
			0xe0a070ff, // Char literal
			0x6a7384ff, // Punctuation
			0x808040ff,	// Preprocessor
			0xdcdfe4ff, // Identifier
			0x61afefff, // Known identifier
			0xc678ddff, // Preproc identifier
			0x3696a2ff, // Comment (single line)
			0x3696a2ff, // Comment (multi line)
			0x282c34ff, // Background
			0xe0e0e0ff, // Cursor
			0x2060a080, // Selection
			0xff200080, // ErrorMarker
			0xffffff15, // ControlCharacter
			0x0080f040, // Breakpoint
			0x7a8394ff, // Line number
			0x00000040, // Current line fill
			0x80808040, // Current line fill (inactive)
			0xa0a0a040, // Current line edge
		} };
	return p;
}

const TextEditor::Palette& TextEditor::GetMarianaPalette()
{
	const static Palette p = { {
			0xffffffff,	// Default
			0xc695c6ff,	// Keyword
			0xf9ae58ff,	// Number
			0x99c794ff,	// String
			0xe0a070ff, // Char literal
			0x5fb4b4ff, // Punctuation
			0x808040ff,	// Preprocessor
			0xffffffff, // Identifier
			0x4dc69bff, // Known identifier
			0xe0a0ffff, // Preproc identifier
			0xa6acb9ff, // Comment (single line)
			0xa6acb9ff, // Comment (multi line)
			0x303841ff, // Background
			0xe0e0e0ff, // Cursor
			0x6e7a8580, // Selection
			0xec5f6680, // ErrorMarker
			0xffffff30, // ControlCharacter
			0x0080f040, // Breakpoint
			0xffffffb0, // Line number
			0x4e5a6580, // Current line fill
			0x4e5a6530, // Current line fill (inactive)
			0x4e5a65b0, // Current line edge
		} };
	return p;
}

const TextEditor::Palette& TextEditor::GetLightPalette()
{
	const static Palette p = { {
			0x404040ff,	// None
			0x060cffff,	// Keyword	
			0x008000ff,	// Number
			0xa02020ff,	// String
			0x704030ff, // Char literal
			0x000000ff, // Punctuation
			0x606040ff,	// Preprocessor
			0x404040ff, // Identifier
			0x106060ff, // Known identifier
			0xa040c0ff, // Preproc identifier
			0x205020ff, // Comment (single line)
			0x205040ff, // Comment (multi line)
			0xffffffff, // Background
			0x000000ff, // Cursor
			0x00006040, // Selection
			0xff1000a0, // ErrorMarker
			0x90909090, // ControlCharacter
			0x0080f080, // Breakpoint
			0x005050ff, // Line number
			0x00000040, // Current line fill
			0x80808040, // Current line fill (inactive)
			0x00000040, // Current line edge
		} };
	return p;
}

const TextEditor::Palette& TextEditor::GetRetroBluePalette()
{
	const static Palette p = { {
			0xffff00ff,	// None
			0x00ffffff,	// Keyword	
			0x00ff00ff,	// Number
			0x008080ff,	// String
			0x008080ff, // Char literal
			0xffffffff, // Punctuation
			0x008000ff,	// Preprocessor
			0xffff00ff, // Identifier
			0xffffffff, // Known identifier
			0xff00ffff, // Preproc identifier
			0x808080ff, // Comment (single line)
			0x404040ff, // Comment (multi line)
			0x000080ff, // Background
			0xff8000ff, // Cursor
			0x00ffff80, // Selection
			0xff0000a0, // ErrorMarker
			0x0080ff80, // Breakpoint
			0x008080ff, // Line number
			0x00000040, // Current line fill
			0x80808040, // Current line fill (inactive)
			0x00000040, // Current line edge
		} };
	return p;
}

const std::unordered_map<char, char> TextEditor::OPEN_TO_CLOSE_CHAR = {
	{'{', '}'},
	{'(' , ')'},
	{'[' , ']'}
};
const std::unordered_map<char, char> TextEditor::CLOSE_TO_OPEN_CHAR = {
	{'}', '{'},
	{')' , '('},
	{']' , '['}
};

TextEditor::PaletteId TextEditor::defaultPalette = TextEditor::PaletteId::Dark;

// Auto-complete implementation
std::string TextEditor::GetCurrentWord() const
{
	auto coords = GetSanitizedCursorCoordinates();
	return GetWordAt(coords);
}

std::string TextEditor::GetWordAt(const Coordinates& aCoords) const
{
	if (aCoords.mLine >= (int)mLines.size())
		return "";
	
	auto& line = mLines[aCoords.mLine];
	int charIndex = GetCharacterIndexL(aCoords);
	
	// Find word start
	int wordStart = charIndex;
	while (wordStart > 0 && CharIsWordChar(line[wordStart - 1].mChar))
		wordStart--;
	
	// Find word end
	int wordEnd = charIndex;
	while (wordEnd < (int)line.size() && CharIsWordChar(line[wordEnd].mChar))
		wordEnd++;
	
	// Extract word
	std::string word;
	for (int i = wordStart; i < wordEnd; i++)
		word += line[i].mChar;
	
	return word;
}

void TextEditor::UpdateAutoComplete()
{
	// Get current cursor position and word
	auto coords = GetSanitizedCursorCoordinates();
	if (coords.mLine >= (int)mLines.size())
	{
		mShowAutoComplete = false;
		return;
	}
	
	auto& line = mLines[coords.mLine];
	int charIndex = GetCharacterIndexL(coords);
	
	// Find word boundaries
	int wordStart = charIndex;
	while (wordStart > 0 && CharIsWordChar(line[wordStart - 1].mChar))
		wordStart--;
	
	mAutoCompleteWordStart = Coordinates(coords.mLine, GetCharacterColumn(coords.mLine, wordStart));
	mAutoCompleteWordEnd = coords;
	
	// Extract current partial word
	std::string currentWord;
	for (int i = wordStart; i < charIndex; i++)
		currentWord += line[i].mChar;
	
	if (currentWord.empty())
	{
		mShowAutoComplete = false;
		mAutoCompleteSuggestions.clear();
		return;
	}
	
	// Convert to uppercase for comparison if language is case-insensitive
	std::string searchWord = currentWord;
	if (mLanguageDefinition && !mLanguageDefinition->mCaseSensitive)
	{
		std::transform(searchWord.begin(), searchWord.end(), searchWord.begin(), ::toupper);
	}
	
	// Find matching keywords
	mAutoCompleteSuggestions.clear();
	
	// Check keywords from language definition
	if (mLanguageDefinition)
	{
		for (const auto& keyword : mLanguageDefinition->mKeywords)
		{
			std::string compareKeyword = keyword;
			if (!mLanguageDefinition->mCaseSensitive)
			{
				std::transform(compareKeyword.begin(), compareKeyword.end(), compareKeyword.begin(), ::toupper);
			}
			
			if (compareKeyword.find(searchWord) == 0 && compareKeyword != searchWord)
			{
				mAutoCompleteSuggestions.push_back(keyword);
			}
		}
	}
	
	// Check extra keywords (table and column names) - case insensitive
	for (const auto& keyword : mExtraKeywords)
	{
		std::string upperKeyword = keyword;
		std::transform(upperKeyword.begin(), upperKeyword.end(), upperKeyword.begin(), ::toupper);
		if (upperKeyword.find(searchWord) == 0 && upperKeyword != searchWord)
		{
			mAutoCompleteSuggestions.push_back(keyword);
		}
	}
	
	// Show auto-complete if we have suggestions
	if (!mAutoCompleteSuggestions.empty())
	{
		mShowAutoComplete = true;
		mAutoCompleteSelectedIndex = 0;
	}
	else
	{
		mShowAutoComplete = false;
		mAutoCompleteSelectedIndex = -1;
	}
}

void TextEditor::RenderAutoComplete()
{
	if (!mShowAutoComplete || mAutoCompleteSuggestions.empty())
		return;
	
	// Calculate popup position based on cursor
	auto cursorCoords = GetSanitizedCursorCoordinates();
	float cursorX = mTextStart + TextDistanceToLineStart(cursorCoords);
	float cursorY = cursorCoords.mLine * mCharAdvance.y;
	
	ImVec2 windowPos = ImGui::GetWindowPos();
	ImVec2 cursorScreenPos = ImVec2(windowPos.x + cursorX - mScrollX, 
	                                windowPos.y + cursorY - mScrollY + mCharAdvance.y);
	
	// Set popup position
	ImGui::SetNextWindowPos(cursorScreenPos);
	
	// Calculate popup size
	float maxWidth = 200.0f;
	float itemHeight = ImGui::GetTextLineHeightWithSpacing();
	float maxHeight = std::min(10.0f, (float)mAutoCompleteSuggestions.size()) * itemHeight + 8.0f;
	
	ImGui::SetNextWindowSize(ImVec2(maxWidth, maxHeight));
	
	// Popup window flags
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | 
	                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
	                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;
	
	// Render popup
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
	if (ImGui::Begin("##SQLAutoComplete", nullptr, flags))
	{
		for (int i = 0; i < (int)mAutoCompleteSuggestions.size(); i++)
		{
			bool isSelected = (i == mAutoCompleteSelectedIndex);
			
			if (ImGui::Selectable(mAutoCompleteSuggestions[i].c_str(), isSelected))
			{
				mAutoCompleteSelectedIndex = i;
				AcceptAutoComplete();
			}
			
			if (isSelected)
			{
				ImGui::SetItemDefaultFocus();
				ImGui::SetScrollHereY();
			}
		}
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

void TextEditor::AcceptAutoComplete()
{
	if (!mShowAutoComplete || mAutoCompleteSelectedIndex < 0 || 
	    mAutoCompleteSelectedIndex >= (int)mAutoCompleteSuggestions.size())
		return;
	
	// Get the selected suggestion
	const std::string& suggestion = mAutoCompleteSuggestions[mAutoCompleteSelectedIndex];
	
	// Delete the current partial word
	DeleteRange(mAutoCompleteWordStart, mAutoCompleteWordEnd);
	
	// Insert the suggestion
	auto pos = mAutoCompleteWordStart;
	InsertTextAt(pos, (suggestion + " ").c_str());
	
	// Update cursor position
	SetCursorPosition(pos);
	
	// Hide auto-complete
	mShowAutoComplete = false;
	mAutoCompleteSuggestions.clear();
	mAutoCompleteSelectedIndex = -1;
	
	// Trigger colorization
	Colorize(mAutoCompleteWordStart.mLine, 1);
}

void TextEditor::RenderFindReplacePanel(const ImVec2& aOrigin, const ImVec2& aSize, bool /*aParentIsFocused*/)
{
	ImGuiIO& io = ImGui::GetIO();
	if (!mShowFindPanel)
	{
		if (mFindStatusTimer > 0.0f)
		{
			mFindStatusTimer = std::max(0.0f, mFindStatusTimer - io.DeltaTime);
			if (mFindStatusTimer <= 0.0f)
				mFindStatusMessage.clear();
		}
		return;
	}

	EnsureFindResultsUpToDate();

	const float padding = 12.0f;
	float panelWidth = std::min(420.0f, aSize.x - padding * 2.0f);
	panelWidth = std::max(panelWidth, 260.0f);
	ImVec2 panelPos(aOrigin.x + aSize.x - panelWidth - padding, aOrigin.y + padding);
	panelPos.x = std::max(panelPos.x, aOrigin.x + padding);

	ImGui::SetNextWindowPos(panelPos);
	ImGui::SetNextWindowSize(ImVec2(panelWidth, 0.0f));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_AlwaysAutoResize;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
	ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(bg.x, bg.y, bg.z, bg.w * 0.98f));

	if (ImGui::Begin("##TextEditorFindReplace", &mShowFindPanel, flags))
	{
		if (mFindStatusTimer > 0.0f)
		{
			mFindStatusTimer = std::max(0.0f, mFindStatusTimer - io.DeltaTime);
			if (mFindStatusTimer <= 0.0f)
				mFindStatusMessage.clear();
		}

		const ImGuiStyle& style = ImGui::GetStyle();
		ImVec4 accent = style.Colors[ImGuiCol_ButtonHovered];
		ImVec4 activeColor = ImVec4(accent.x, accent.y, accent.z, 0.85f);
		ImVec4 inactiveColor = style.Colors[ImGuiCol_FrameBg];
		ImVec4 inactiveHover = ImVec4(inactiveColor.x, inactiveColor.y, inactiveColor.z, inactiveColor.w + 0.1f);

		int matchCount = (int)mFindResults.size();
		int currentMatch = (matchCount > 0 && mFindResultIndex >= 0) ? (mFindResultIndex + 1) : 0;
		bool hasPattern = HasValidFindPattern();

		auto drawToggle = [&](const char* id, const char* label, bool* value, bool disabled, const char* tooltip) -> bool
		{
			bool changed = false;
			ImGui::BeginDisabled(disabled);
			ImGui::PushID(id);
			ImGui::PushStyleColor(ImGuiCol_Button, *value ? activeColor : inactiveColor);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, *value ? activeColor : inactiveHover);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, *value ? activeColor : inactiveHover);
			ImVec2 labelSize = ImGui::CalcTextSize(label);
			float buttonWidth = labelSize.x + style.FramePadding.x * 2.0f;
			if (ImGui::Button(label, ImVec2(buttonWidth, 0.0f)))
			{
				*value = !*value;
				changed = true;
			}
			if (tooltip && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", tooltip);
			ImGui::PopStyleColor(3);
			ImGui::PopID();
			ImGui::EndDisabled();
			return changed;
		};

		ImGui::PushID("FindReplaceModern");
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 6.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 6.0f));

		const ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoBordersInBody;
		if (ImGui::BeginTable("FindHeader", 4, tableFlags))
		{
			ImGui::TableSetupColumn("Toggles", ImGuiTableColumnFlags_WidthFixed, 120.0f);
			ImGui::TableSetupColumn("SearchInput", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Navigation", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			ImGui::TableSetupColumn("Close", ImGuiTableColumnFlags_WidthFixed, 24.0f);

			ImGui::TableNextRow();

			// Toggle cluster
			ImGui::TableSetColumnIndex(0);
			ImGui::BeginGroup();
			if (drawToggle("match_case", "Aa", &mFindCaseSensitive, false, "Match case"))
				MarkFindResultsDirty(false);
			ImGui::SameLine();
			bool wholeWordDisabled = mFindUseRegex;
			if (drawToggle("whole_word", "wd", &mFindWholeWord, wholeWordDisabled, "Whole word"))
				MarkFindResultsDirty(false);
			if (wholeWordDisabled)
				mFindWholeWord = false;
			ImGui::SameLine();
			bool regexChanged = drawToggle("use_regex", ".*", &mFindUseRegex, false, "Regular expression");
			if (regexChanged)
			{
				MarkFindResultsDirty(false);
				if (mFindUseRegex)
					mFindWholeWord = false;
			}
			ImGui::EndGroup();

			// Search input
			ImGui::TableSetColumnIndex(1);
			if (mFindFocusRequested)
			{
				ImGui::SetKeyboardFocusHere();
				mFindFocusRequested = false;
			}
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGuiInputTextFlags findFlags = ImGuiInputTextFlags_EnterReturnsTrue;
			if (ImGui::InputTextWithHint("##FindInput", "Search...", mFindBuffer, IM_ARRAYSIZE(mFindBuffer), findFlags))
				MarkFindResultsDirty(true);
			if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Enter))
				FindNext(io.KeyShift);

			// Navigation controls
			ImGui::TableSetColumnIndex(2);
			ImGui::BeginDisabled(!hasPattern || matchCount == 0);
			ImVec2 arrowSize(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
			if (ImGui::Button("<", arrowSize))
				FindNext(true);
			ImGui::SameLine();
			if (ImGui::Button(">", arrowSize))
				FindNext(false);
			ImGui::SameLine();
			ImGui::Text("%d/%d", currentMatch, matchCount);
			ImGui::EndDisabled();

			// Close button
			ImGui::TableSetColumnIndex(3);
			if (ImGui::Button("x"))
				mShowFindPanel = false;

			ImGui::EndTable();
		}

		// Replace field
		if (mReplaceFocusRequested)
		{
			ImGui::SetKeyboardFocusHere();
			mReplaceFocusRequested = false;
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::InputTextWithHint("##ReplaceInput", "Replace with...", mReplaceBuffer, IM_ARRAYSIZE(mReplaceBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
		{
			// no-op, buffer already updated
		}
		if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Enter))
			ReplaceCurrent();

		// Additional toggles row
		ImGui::Spacing();
		ImGui::BeginGroup();
		bool wrapChanged = drawToggle("wrap_toggle", "Wrap", &mFindWrapAround, false, "Wrap search");
		if (wrapChanged)
			MarkFindResultsDirty(false);
		ImGui::SameLine();
		bool selectionChanged = drawToggle("selection_toggle", "Sel", &mFindSelectionOnly, false, "Limit search to selection");
		if (selectionChanged)
		{
			if (mFindSelectionOnly)
			{
				Coordinates selStart;
				Coordinates selEnd;
				if (TryGetSelectionBounds(selStart, selEnd))
				{
					mFindSelectionRangeStart = selStart;
					mFindSelectionRangeEnd = selEnd;
					mFindSelectionRangeValid = true;
				}
				else
				{
					mFindSelectionOnly = false;
					mFindSelectionRangeValid = false;
					mFindStatusMessage = "Select text to limit search";
					mFindStatusTimer = 2.0f;
				}
			}
			else
			{
				mFindSelectionRangeValid = false;
			}
			MarkFindResultsDirty(false);
		}
		ImGui::EndGroup();

		float replaceWidth = ImGui::CalcTextSize("Replace").x + style.FramePadding.x * 2.0f;
		float replaceAllWidth = ImGui::CalcTextSize("Replace All").x + style.FramePadding.x * 2.0f;
		float actionTotalWidth = replaceWidth + style.ItemSpacing.x + replaceAllWidth;
		float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		float actionStart = rightEdge - actionTotalWidth;
		ImGui::SameLine(std::max(ImGui::GetCursorPosX(), actionStart));
		ImGui::BeginDisabled(!hasPattern || matchCount == 0 || mReadOnly);
				if (ImGui::Button("Replace"))
				ReplaceCurrent();
		ImGui::SameLine();
		if (ImGui::Button("Replace All"))
			ReplaceAll();
		ImGui::EndDisabled();

		ImGui::PopStyleVar(3);
		ImGui::PopID();

		if (!mFindStatusMessage.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextUnformatted(mFindStatusMessage.c_str());
			ImGui::PopStyleColor();
		}
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
}
