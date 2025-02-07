//------------------------------------------------------------------------------
//! @file LexerFacts.h
//! @brief Random lexer-related utility functions
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#pragma once

#include "slang/util/StringTable.h"

namespace slang {

enum class SyntaxKind;
enum class TokenKind : uint16_t;

/// Different restricted sets of keywords that can be set using the
/// `begin_keywords directive. The values of the enum correspond to indexes to
/// allKeywords[] in LexerFacts.cpp
enum class KeywordVersion : uint8_t {
    v1364_1995 = 0,
    v1364_2001_noconfig = 1,
    v1364_2001 = 2,
    v1364_2005 = 3,
    v1800_2005 = 4,
    v1800_2009 = 5,
    v1800_2012 = 6,
    v1800_2017 = 7,
};

class LexerFacts {
public:
    static TokenKind getSystemKeywordKind(string_view text);
    static string_view getTokenKindText(TokenKind kind);
    static KeywordVersion getDefaultKeywordVersion();
    static optional<KeywordVersion> getKeywordVersion(string_view text);
    static const StringTable<TokenKind>* getKeywordTable(KeywordVersion version);

    static SyntaxKind getDirectiveKind(string_view directive);
    static string_view getDirectiveText(SyntaxKind kind);

    /// This checks all keywords, regardless of the current keyword table. Should
    /// only be used when it is ok to get a false positive for a keyword that may
    /// not currently be in the keyword table (such as handling macro arguments).
    static bool isKeyword(TokenKind kind);

private:
    LexerFacts() = default;
};

} // namespace slang
