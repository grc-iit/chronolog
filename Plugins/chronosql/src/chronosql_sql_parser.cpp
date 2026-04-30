#include "chronosql_sql_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace chronosql
{
namespace
{

enum class Tok
{
    Ident,
    StringLit,
    Integer,
    Decimal,
    LParen,
    RParen,
    Comma,
    Semicolon,
    Equals,
    Star,
    End
};

struct Token
{
    Tok kind;
    std::string text;     // identifier text or numeric digits (lowercased for identifiers)
    std::string original; // original-case text for error messages
    std::size_t pos{0};
};

bool iequals(const std::string& a, const char* b)
{
    std::size_t n = std::char_traits<char>::length(b);
    if(a.size() != n)
    {
        return false;
    }
    for(std::size_t i = 0; i < n; ++i)
    {
        if(std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

class Tokenizer
{
public:
    explicit Tokenizer(const std::string& src)
        : src_(src)
    {}

    std::vector<Token> tokenize()
    {
        std::vector<Token> out;
        while(pos_ < src_.size())
        {
            char c = src_[pos_];
            if(std::isspace(static_cast<unsigned char>(c)))
            {
                ++pos_;
                continue;
            }
            if(c == ',')
            {
                out.push_back({Tok::Comma, ",", ",", pos_});
                ++pos_;
                continue;
            }
            if(c == '(')
            {
                out.push_back({Tok::LParen, "(", "(", pos_});
                ++pos_;
                continue;
            }
            if(c == ')')
            {
                out.push_back({Tok::RParen, ")", ")", pos_});
                ++pos_;
                continue;
            }
            if(c == ';')
            {
                out.push_back({Tok::Semicolon, ";", ";", pos_});
                ++pos_;
                continue;
            }
            if(c == '=')
            {
                out.push_back({Tok::Equals, "=", "=", pos_});
                ++pos_;
                continue;
            }
            if(c == '*')
            {
                out.push_back({Tok::Star, "*", "*", pos_});
                ++pos_;
                continue;
            }
            if(c == '\'')
            {
                out.push_back(readString());
                continue;
            }
            if(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+')
            {
                // Disambiguate +/-: only treat as numeric prefix if next char is a digit.
                if((c == '-' || c == '+') &&
                   (pos_ + 1 >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_ + 1]))))
                {
                    error("unexpected character", pos_);
                }
                out.push_back(readNumber());
                continue;
            }
            if(std::isalpha(static_cast<unsigned char>(c)) || c == '_')
            {
                out.push_back(readIdent());
                continue;
            }
            error("unexpected character", pos_);
        }
        out.push_back({Tok::End, "", "", pos_});
        return out;
    }

private:
    const std::string& src_;
    std::size_t pos_{0};

    [[noreturn]] void error(const std::string& msg, std::size_t at)
    {
        throw std::invalid_argument("ChronoSQL parse error at offset " + std::to_string(at) + ": " + msg);
    }

    Token readString()
    {
        std::size_t start = pos_;
        ++pos_; // consume opening quote
        std::string out;
        while(pos_ < src_.size())
        {
            char c = src_[pos_];
            if(c == '\'')
            {
                if(pos_ + 1 < src_.size() && src_[pos_ + 1] == '\'')
                {
                    out += '\'';
                    pos_ += 2;
                    continue;
                }
                ++pos_;
                return {Tok::StringLit, out, out, start};
            }
            out += c;
            ++pos_;
        }
        error("unterminated string literal", start);
    }

    Token readNumber()
    {
        std::size_t start = pos_;
        if(src_[pos_] == '-' || src_[pos_] == '+')
        {
            ++pos_;
        }
        bool seen_dot = false;
        while(pos_ < src_.size())
        {
            char c = src_[pos_];
            if(std::isdigit(static_cast<unsigned char>(c)))
            {
                ++pos_;
                continue;
            }
            if(c == '.' && !seen_dot)
            {
                seen_dot = true;
                ++pos_;
                continue;
            }
            break;
        }
        std::string lex = src_.substr(start, pos_ - start);
        return {seen_dot ? Tok::Decimal : Tok::Integer, lex, lex, start};
    }

    Token readIdent()
    {
        std::size_t start = pos_;
        while(pos_ < src_.size())
        {
            char c = src_[pos_];
            if(std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            {
                ++pos_;
                continue;
            }
            break;
        }
        std::string original = src_.substr(start, pos_ - start);
        std::string lower = original;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return std::tolower(ch); });
        return {Tok::Ident, lower, original, start};
    }
};

class Parser
{
public:
    explicit Parser(std::vector<Token> tokens)
        : toks_(std::move(tokens))
    {}

    ParsedStatement parse()
    {
        const Token& kw = peek();
        if(kw.kind != Tok::Ident)
        {
            fail("expected SQL keyword", kw);
        }
        ParsedStatement out;
        if(iequals(kw.text, "create"))
        {
            out = parseCreate();
        }
        else if(iequals(kw.text, "drop"))
        {
            out = parseDrop();
        }
        else if(iequals(kw.text, "insert"))
        {
            out = parseInsert();
        }
        else if(iequals(kw.text, "select"))
        {
            out = parseSelect();
        }
        else
        {
            fail("unsupported statement '" + kw.original + "'", kw);
        }
        // Optional trailing semicolon.
        if(peek().kind == Tok::Semicolon)
        {
            ++idx_;
        }
        if(peek().kind != Tok::End)
        {
            fail("trailing tokens after statement", peek());
        }
        return out;
    }

private:
    std::vector<Token> toks_;
    std::size_t idx_{0};

    const Token& peek() const { return toks_[idx_]; }

    [[noreturn]] void fail(const std::string& msg, const Token& t)
    {
        throw std::invalid_argument("ChronoSQL parse error at offset " + std::to_string(t.pos) + ": " + msg);
    }

    void expectKeyword(const char* kw)
    {
        const Token& t = peek();
        if(t.kind != Tok::Ident || !iequals(t.text, kw))
        {
            fail(std::string("expected '") + kw + "'", t);
        }
        ++idx_;
    }

    Token expectIdent()
    {
        const Token& t = peek();
        if(t.kind != Tok::Ident)
        {
            fail("expected identifier", t);
        }
        ++idx_;
        return t;
    }

    void expect(Tok kind, const char* what)
    {
        const Token& t = peek();
        if(t.kind != kind)
        {
            fail(std::string("expected '") + what + "'", t);
        }
        ++idx_;
    }

    ParsedCreateTable parseCreate()
    {
        ++idx_; // consume CREATE
        expectKeyword("table");
        ParsedCreateTable out;
        out.table = expectIdent().original;
        expect(Tok::LParen, "(");
        while(true)
        {
            Column col;
            col.name = expectIdent().original;
            // optional type identifier
            const Token& maybeType = peek();
            if(maybeType.kind == Tok::Ident)
            {
                std::string t = maybeType.original;
                ++idx_;
                if(peek().kind == Tok::LParen)
                {
                    // accept VARCHAR(N) and similar; ignore length.
                    ++idx_;
                    if(peek().kind != Tok::Integer)
                    {
                        fail("expected integer length", peek());
                    }
                    ++idx_;
                    expect(Tok::RParen, ")");
                }
                auto resolved = columnTypeFromString(t);
                if(!resolved)
                {
                    fail("unknown column type '" + t + "'", maybeType);
                }
                col.type = *resolved;
            }
            out.columns.push_back(std::move(col));
            if(peek().kind == Tok::Comma)
            {
                ++idx_;
                continue;
            }
            break;
        }
        expect(Tok::RParen, ")");
        return out;
    }

    ParsedDropTable parseDrop()
    {
        ++idx_; // DROP
        expectKeyword("table");
        ParsedDropTable out;
        out.table = expectIdent().original;
        return out;
    }

    ParsedInsert parseInsert()
    {
        ++idx_; // INSERT
        expectKeyword("into");
        ParsedInsert out;
        out.table = expectIdent().original;
        if(peek().kind == Tok::LParen)
        {
            ++idx_;
            while(true)
            {
                out.columns.push_back(expectIdent().original);
                if(peek().kind == Tok::Comma)
                {
                    ++idx_;
                    continue;
                }
                break;
            }
            expect(Tok::RParen, ")");
        }
        expectKeyword("values");
        expect(Tok::LParen, "(");
        while(true)
        {
            const Token& v = peek();
            if(v.kind == Tok::Ident && iequals(v.text, "null"))
            {
                ++idx_;
                out.values.push_back("");
                out.is_null.push_back(true);
            }
            else if(v.kind == Tok::StringLit || v.kind == Tok::Integer || v.kind == Tok::Decimal)
            {
                ++idx_;
                out.values.push_back(v.text);
                out.is_null.push_back(false);
            }
            else
            {
                fail("expected value literal", v);
            }
            if(peek().kind == Tok::Comma)
            {
                ++idx_;
                continue;
            }
            break;
        }
        expect(Tok::RParen, ")");
        if(!out.columns.empty() && out.columns.size() != out.values.size())
        {
            fail("INSERT column count does not match value count", peek());
        }
        return out;
    }

    ParsedSelect parseSelect()
    {
        ++idx_; // SELECT
        ParsedSelect out;
        if(peek().kind == Tok::Star)
        {
            ++idx_;
        }
        else
        {
            while(true)
            {
                out.projection.push_back(expectIdent().original);
                if(peek().kind == Tok::Comma)
                {
                    ++idx_;
                    continue;
                }
                break;
            }
        }
        expectKeyword("from");
        out.table = expectIdent().original;
        if(peek().kind == Tok::Ident && iequals(peek().text, "where"))
        {
            ++idx_;
            const Token& lhs = peek();
            if(lhs.kind != Tok::Ident)
            {
                fail("expected column in WHERE", lhs);
            }
            // __ts BETWEEN n AND n
            if(lhs.text == "__ts" && idx_ + 1 < toks_.size() && toks_[idx_ + 1].kind == Tok::Ident &&
               iequals(toks_[idx_ + 1].text, "between"))
            {
                ++idx_; // __ts
                ++idx_; // BETWEEN
                const Token& lo = peek();
                if(lo.kind != Tok::Integer)
                {
                    fail("expected integer for BETWEEN lower bound", lo);
                }
                ++idx_;
                expectKeyword("and");
                const Token& hi = peek();
                if(hi.kind != Tok::Integer)
                {
                    fail("expected integer for BETWEEN upper bound", hi);
                }
                ++idx_;
                WhereTsBetween w;
                try
                {
                    w.lo = std::stoull(lo.text);
                    w.hi = std::stoull(hi.text);
                }
                catch(const std::exception&)
                {
                    fail("BETWEEN bounds out of range", lo);
                }
                out.where = w;
            }
            else
            {
                WhereEq w;
                w.column = expectIdent().original;
                expect(Tok::Equals, "=");
                const Token& v = peek();
                if(v.kind == Tok::Ident && iequals(v.text, "null"))
                {
                    ++idx_;
                    w.is_null = true;
                }
                else if(v.kind == Tok::StringLit || v.kind == Tok::Integer || v.kind == Tok::Decimal)
                {
                    ++idx_;
                    w.value = v.text;
                }
                else
                {
                    fail("expected literal value in WHERE", v);
                }
                out.where = w;
            }
        }
        return out;
    }
};

} // namespace

ParsedStatement parseStatement(const std::string& sql)
{
    Tokenizer tk(sql);
    auto tokens = tk.tokenize();
    Parser p(std::move(tokens));
    return p.parse();
}

} // namespace chronosql
