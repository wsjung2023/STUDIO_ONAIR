#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace creator::cut_suggest {

/// Deterministic, ASCII-oriented text normalization shared by the filler
/// lexicon and the filler detector, so a lexicon entry and a transcript word
/// are compared on exactly the same footing (CLAUDE.md 8: determinism).
///
/// Rules (documented so matching is explainable):
///   - ASCII 'A'..'Z' are lowercased; every other byte, including UTF-8
///     continuation bytes of non-ASCII letters, is left untouched. Filler words
///     are English function words, so ASCII folding is sufficient and never
///     corrupts a multi-byte codepoint.
///   - Leading and trailing bytes that are not ASCII alphanumeric are stripped
///     (so "Um," and "(uh)" fold to "um"/"uh"). Interior bytes are kept, so an
///     apostrophe inside a word survives.
namespace text_normalize {

[[nodiscard]] inline bool isAsciiAlnum(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

[[nodiscard]] inline bool isAsciiSpace(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

/// Lowercase + strip surrounding punctuation from a single token. Returns an
/// empty string if nothing alphanumeric remains.
[[nodiscard]] inline std::string normalizeToken(std::string_view raw) {
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && !isAsciiAlnum(static_cast<unsigned char>(raw[begin]))) {
        ++begin;
    }
    while (end > begin && !isAsciiAlnum(static_cast<unsigned char>(raw[end - 1]))) {
        --end;
    }
    std::string out;
    out.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c - 'A' + 'a');
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

/// Split on ASCII whitespace, normalize each token, and drop tokens that
/// normalize to empty. Used to fold a lexicon phrase ("You know") into its
/// canonical token list ("you", "know").
[[nodiscard]] inline std::vector<std::string> normalizePhrase(std::string_view raw) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && isAsciiSpace(static_cast<unsigned char>(raw[i]))) {
            ++i;
        }
        std::size_t start = i;
        while (i < raw.size() && !isAsciiSpace(static_cast<unsigned char>(raw[i]))) {
            ++i;
        }
        if (i > start) {
            std::string token = normalizeToken(raw.substr(start, i - start));
            if (!token.empty()) {
                tokens.push_back(std::move(token));
            }
        }
    }
    return tokens;
}

}  // namespace text_normalize

}  // namespace creator::cut_suggest
