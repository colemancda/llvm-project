//===--- Token.h - Tokens and token streams in the pseudoparser --*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tokens are the first level of abstraction above bytes used in pseudoparsing.
// We use clang's lexer to scan the bytes (in raw mode, with no preprocessor).
// The tokens is wrapped into pseudo::Token, along with line/indent info.
//
// Unlike clang, we make multiple passes over the whole file, out-of-order.
// Therefore we retain the whole token sequence in memory. (This is feasible as
// we process one file at a time). pseudo::TokenStream holds such a stream.
// The initial stream holds the raw tokens read from the file, later passes
// operate on derived TokenStreams (e.g. with directives stripped).
//
// Similar facilities from clang that are *not* used:
//  - SourceManager: designed around multiple files and precise macro expansion.
//  - clang::Token: coupled to SourceManager, doesn't retain layout info.
//                  (pseudo::Token is similar, but without SourceLocations).
//  - syntax::TokenBuffer: coupled to SourceManager, has #includes and macros.
//                  (pseudo::TokenStream is similar, but a flat token list).
//
//===----------------------------------------------------------------------===//

#ifndef CLANG_PSEUDO_TOKEN_H
#define CLANG_PSEUDO_TOKEN_H

#include "clang/Basic/LLVM.h"
#include "clang/Basic/TokenKinds.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace clang {
class LangOptions;
namespace pseudo {

/// A single C++ or preprocessor token.
///
/// Unlike clang::Token and syntax::Token, these tokens are not connected to a
/// SourceManager - we are not dealing with multiple files.
struct Token {
  /// An Index identifies a token within a stream.
  using Index = uint32_t;
  /// A sentinel Index indicating no token.
  constexpr static Index Invalid = std::numeric_limits<Index>::max();
  struct Range;

  /// The token text.
  ///
  /// Typically from the original source file, but may have been synthesized.
  StringRef text() const { return StringRef(Data, Length); }
  const char *Data = nullptr;
  uint32_t Length = 0;

  /// Zero-based line number for the start of the token.
  /// This refers to the original source file as written.
  uint32_t Line = 0;
  /// Width of whitespace before the first token on this line.
  uint8_t Indent = 0;
  /// Flags have some meaning defined by the function that produced this stream.
  uint8_t Flags = 0;
  // Helpers to get/set Flags based on `enum class`.
  template <class T> bool flag(T Mask) const {
    return Flags & uint8_t{static_cast<std::underlying_type_t<T>>(Mask)};
  }
  template <class T> void setFlag(T Mask) {
    Flags |= uint8_t{static_cast<std::underlying_type_t<T>>(Mask)};
  }

  /// The type of token as determined by clang's lexer.
  clang::tok::TokenKind Kind = clang::tok::unknown;
};
static_assert(sizeof(Token) <= sizeof(char *) + 16, "Careful with layout!");
llvm::raw_ostream &operator<<(llvm::raw_ostream &, const Token &);

/// A half-open range of tokens within a stream.
struct Token::Range {
  Index Begin = 0;
  Index End = 0;

  uint32_t size() const { return End - Begin; }
  static Range emptyAt(Index Index) { return Range{Index, Index}; }
};
llvm::raw_ostream &operator<<(llvm::raw_ostream &, const Token::Range &);

/// A complete sequence of Tokens representing a source file.
///
/// This may match a raw file from disk, or be derived from a previous stream.
/// For example, stripping comments from a TokenStream results in a new stream.
///
/// A stream has sentinel 'eof' tokens at each end, e.g `int main();` becomes:
///       int      main   (        )        ;
///   eof kw_int   ident  l_paren  r_paren  semi   eof
///       front()                           back()
///       0        1      2        3        4      5
class TokenStream {
public:
  /// Create an empty stream.
  ///
  /// Initially, the stream is appendable and not finalized.
  /// The token sequence may only be accessed after finalize() is called.
  ///
  /// Payload is an opaque object which will be owned by the stream.
  /// e.g. an allocator to hold backing storage for synthesized token text.
  explicit TokenStream(std::shared_ptr<void> Payload = nullptr);

  /// Append a token to the stream, which must not be finalized.
  void push(Token T) {
    assert(!isFinalized());
    Storage.push_back(std::move(T));
  }

  /// Finalize the token stream, allowing tokens to be accessed.
  /// Tokens may no longer be appended.
  void finalize();
  bool isFinalized() const;

  /// Returns the index of T within the stream.
  ///
  /// T must be within the stream or the end sentinel (not the start sentinel).
  Token::Index index(const Token &T) const {
    assert(isFinalized());
    assert(&T >= Storage.data() && &T < Storage.data() + Storage.size());
    assert(&T != Storage.data() && "start sentinel");
    return &T - Tokens.data();
  }

  ArrayRef<Token> tokens() const {
    assert(isFinalized());
    return Tokens;
  }
  ArrayRef<Token> tokens(Token::Range R) const {
    return tokens().slice(R.Begin, R.End - R.Begin);
  }

  /// May return the end sentinel if the stream is empty.
  const Token &front() const {
    assert(isFinalized());
    return Storage[1];
  }

  /// Print the tokens in this stream to the output stream.
  ///
  /// The presence of newlines/spaces is preserved, but not the quantity.
  void print(llvm::raw_ostream &) const;

private:
  std::shared_ptr<void> Payload;

  MutableArrayRef<Token> Tokens;
  std::vector<Token> Storage; // eof + Tokens + eof
};
llvm::raw_ostream &operator<<(llvm::raw_ostream &, const TokenStream &);

/// Extracts a raw token stream from the source code.
///
/// All tokens will reference the data of the provided string.
/// "word-like" tokens such as identifiers and keywords will be raw_identifier.
TokenStream lex(const std::string &, const clang::LangOptions &);
enum class LexFlags : uint8_t {
  /// Marks the token at the start of a logical preprocessor line.
  /// This is a position where a directive might start.
  ///
  /// Here, the first # is StartsPPLine, but second is not (same logical line).
  ///   #define X(error) \
  ///   #error // not a directive!
  ///
  /// Careful, the directive may not start exactly on the StartsPPLine token:
  ///   /*comment*/ #include <foo.h>
  StartsPPLine = 1 << 0,
  /// Marks tokens containing trigraphs, escaped newlines, UCNs etc.
  /// The text() of such tokens will contain the raw trigrah.
  NeedsCleaning = 1 << 1,
};

/// Derives a token stream by decoding escapes and interpreting raw_identifiers.
///
/// Tokens containing UCNs, escaped newlines, trigraphs etc are decoded and
/// their backing data is owned by the returned stream.
/// raw_identifier tokens are assigned specific types (identifier, keyword etc).
///
/// The StartsPPLine flag is preserved.
///
/// Formally the identifier correctly happens before preprocessing, while we
/// should only cook raw_identifiers that survive preprocessing.
/// However, ignoring the Token::Kind of tokens in directives achieves the same.
/// (And having cooked token kinds in PP-disabled sections is useful for us).
TokenStream cook(const TokenStream &, const clang::LangOptions &);

/// Drops comment tokens.
TokenStream stripComments(const TokenStream &);

} // namespace pseudo
} // namespace clang

#endif
