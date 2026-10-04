#include "chronolog/sql/database.h"
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace chronolog::sql
{
namespace
{
struct Token
{
    std::string text;
    bool quoted{};
};
class Parser
{
    std::vector<Token> tokens_;
    size_t at_{}, parameter_{};
    std::span<const Value> parameters_;
    const Token& token() const { return tokens_.at(at_); }
    [[noreturn]] void fail(const std::string& reason = "unsupported") const
    {
        throw std::invalid_argument(reason + " token '" + token().text + "'");
    }
    bool take(const std::string& s)
    {
        if(!token().quoted && token().text == s)
        {
            ++at_;
            return true;
        }
        return false;
    }
    void need(const std::string& s)
    {
        if(!take(s))
            fail("expected " + s + " at");
    }
    std::string identifier()
    {
        auto t = token();
        if(t.quoted || t.text.empty() || !(std::isalpha(static_cast<unsigned char>(t.text[0])) || t.text[0] == '_'))
            fail();
        for(unsigned char c: t.text)
            if(!std::isalnum(c) && c != '_')
                fail();
        ++at_;
        return t.text;
    }
    Value literal()
    {
        auto t = token();
        if(take("?"))
        {
            if(parameter_ >= parameters_.size())
                throw std::invalid_argument("unbound token '?'");
            return parameters_[parameter_++];
        }
        if(t.quoted)
        {
            ++at_;
            return t.text;
        }
        if(take("null"))
            return nullptr;
        if(take("true"))
            return true;
        if(take("false"))
            return false;
        if(take("x"))
        {
            auto hex = token();
            if(!hex.quoted || hex.text.size() % 2)
                fail("invalid BLOB");
            std::vector<uint8_t> bytes;
            for(size_t i = 0; i < hex.text.size(); i += 2)
            {
                auto digit = [](unsigned char c) -> int
                {
                    if(c >= '0' && c <= '9')
                        return c - '0';
                    c = static_cast<unsigned char>(std::tolower(c));
                    if(c >= 'a' && c <= 'f')
                        return c - 'a' + 10;
                    return -1;
                };
                int a = digit(hex.text[i]), b = digit(hex.text[i + 1]);
                if(a < 0 || b < 0)
                    fail("invalid BLOB");
                bytes.push_back(static_cast<uint8_t>(a * 16 + b));
            }
            ++at_;
            return Value::binary(bytes);
        }
        try
        {
            size_t used{};
            if(t.text.find_first_of(".eE") != std::string::npos)
            {
                double v = std::stod(t.text, &used);
                if(used != t.text.size() || !std::isfinite(v))
                    fail();
                ++at_;
                return v;
            }
            auto v = std::stoll(t.text, &used);
            if(used != t.text.size())
                fail();
            ++at_;
            return v;
        }
        catch(const std::exception&)
        {
            fail("invalid literal");
        }
    }
    Hlc hlc()
    {
        const auto origin = token().text;
        auto value = literal();
        try
        {
            if(!value.is_string())
                throw std::invalid_argument("HLC must be ns:logical");
            auto text = value.get<std::string>();
            auto colon = text.find(':');
            if(colon == std::string::npos || colon + 1 == text.size())
                throw std::invalid_argument("invalid HLC");
            size_t a{}, b{};
            auto ns = std::stoll(text.substr(0, colon), &a);
            auto log = std::stoull(text.substr(colon + 1), &b);
            if(a != colon || b != text.size() - colon - 1 || log > UINT32_MAX || text[colon + 1] == '-')
                throw std::invalid_argument("invalid HLC");
            return {ns, static_cast<uint32_t>(log)};
        }
        catch(const std::exception&)
        {
            throw std::invalid_argument("invalid HLC token '" + origin + "'");
        }
    }

public:
    Parser(const std::string& sql, std::span<const Value> parameters)
        : parameters_(parameters)
    {
        if(sql.size() > (1u << 20))
            throw std::invalid_argument("oversized token SQL");
        for(size_t i = 0; i < sql.size();)
        {
            unsigned char c = static_cast<unsigned char>(sql[i]);
            if(std::isspace(c))
            {
                ++i;
                continue;
            }
            if(c == '\'')
            {
                ++i;
                std::string value;
                bool closed = false;
                while(i < sql.size())
                {
                    if(sql[i] == '\'')
                    {
                        ++i;
                        if(i < sql.size() && sql[i] == '\'')
                        {
                            value += '\'';
                            ++i;
                        }
                        else
                        {
                            closed = true;
                            break;
                        }
                    }
                    else
                        value += sql[i++];
                }
                if(!closed)
                    throw std::invalid_argument("unterminated token '\''");
                tokens_.push_back({value, true});
                continue;
            }
            size_t begin = i++;
            if(std::isalpha(c) || c == '_')
            {
                while(i < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) ++i;
            }
            else if(std::isdigit(c) || c == '-' || c == '+')
            {
                while(i < sql.size() && (std::isdigit(static_cast<unsigned char>(sql[i])) || sql[i] == '.' ||
                                         sql[i] == 'e' || sql[i] == 'E' || sql[i] == '+' || sql[i] == '-'))
                    ++i;
            }
            else if((c == '<' || c == '>' || c == '!') && i < sql.size() && sql[i] == '=')
                ++i;
            std::string text = sql.substr(begin, i - begin);
            for(char& ch: text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            tokens_.push_back({text, false});
        }
        tokens_.push_back({"<end>", false});
    }
    Statement run()
    {
        Statement s{};
        if(take("create"))
        {
            s.kind = Statement::Kind::Create;
            need("table");
            s.table = identifier();
            need("(");
            std::set<std::string> names;
            do {
                auto name = identifier();
                if(name[0] == '_' || !names.insert(name).second)
                    throw std::invalid_argument("invalid column token '" + name + "'");
                auto type = identifier();
                if(type != "integer" && type != "real" && type != "text" && type != "blob" && type != "boolean")
                    throw std::invalid_argument("unsupported type token '" + type + "'");
                s.schema.push_back({name, type});
                if(s.schema.size() > 256)
                    fail("too many columns at");
            } while(take(","));
            need(")");
        }
        else if(take("insert"))
        {
            s.kind = Statement::Kind::Insert;
            need("into");
            s.table = identifier();
            if(take("("))
            {
                do {
                    s.columns.push_back(identifier());
                } while(take(","));
                need(")");
            }
            need("values");
            do {
                need("(");
                std::vector<Value> tuple;
                do {
                    tuple.push_back(literal());
                    if(tuple.size() > 256)
                        fail();
                } while(take(","));
                need(")");
                s.tuples.push_back(std::move(tuple));
                if(s.tuples.size() > 10000)
                    fail();
            } while(take(","));
        }
        else if(take("select"))
        {
            s.kind = Statement::Kind::Select;
            if(take("count"))
            {
                need("(");
                need("*");
                need(")");
                s.count = true;
            }
            else if(!take("*"))
            {
                do {
                    s.columns.push_back(identifier());
                } while(take(","));
            }
            need("from");
            s.table = identifier();
            if(take("where"))
                do {
                    auto col = identifier();
                    if(col == "time")
                    {
                        need("between");
                        auto lo = hlc();
                        need("and");
                        auto hi = hlc();
                        if(hi < lo || s.range || s.physical_range)
                            throw std::invalid_argument("invalid token TIME range");
                        s.range = client::HlcRange{lo, hi};
                    }
                    else if(col == "physical")
                    {
                        need("between");
                        auto lo = literal();
                        need("and");
                        auto hi = literal();
                        if(!lo.is_number_integer() || !hi.is_number_integer() || s.range || s.physical_range)
                            throw std::invalid_argument("invalid token PHYSICAL range");
                        const auto start = lo.get<int64_t>(), end = hi.get<int64_t>();
                        if(end <= start)
                            throw std::invalid_argument("invalid token PHYSICAL range");
                        s.physical_range = client::PhysicalRange{start, end};
                    }
                    else
                    {
                        auto op = token().text;
                        if(token().quoted ||
                           (op != "=" && op != "!=" && op != "<" && op != ">" && op != "<=" && op != ">="))
                            fail();
                        ++at_;
                        s.predicates.push_back({col, op, literal()});
                    }
                } while(take("and"));
            if(take("order"))
            {
                need("by");
                s.order = identifier();
                if(s.order != "time" && s.order != "_hlc" && s.order != "_physical_ns" && s.order != "_event_id")
                    throw std::invalid_argument("unsupported order token '" + s.order + "'");
                s.descending = take("desc");
                if(!s.descending)
                    take("asc");
            }
            if(take("limit"))
            {
                auto v = literal();
                if(!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<int64_t>() > 10000)
                    throw std::invalid_argument("invalid token LIMIT");
                s.limit = v.get<size_t>();
            }
            if(s.descending && !s.limit)
                throw std::invalid_argument("token DESC requires LIMIT");
        }
        else
            fail();
        take(";");
        if(token().quoted || token().text != "<end>")
            fail();
        if(parameter_ != parameters_.size())
            throw std::invalid_argument("unused token '?'");
        return s;
    }
};
} // namespace
absl::StatusOr<Statement> parse(const std::string& sql, std::span<const Value> parameters)
{
    try
    {
        return Parser(sql, parameters).run();
    }
    catch(const std::exception& e)
    {
        return absl::InvalidArgumentError(e.what());
    }
}
std::string encodeRow(const std::vector<Value>& values)
{
    Value row = Value::array();
    for(const auto& v: values)
    {
        if(v.is_binary())
            row.push_back({{"blob", std::vector<uint8_t>(v.get_binary().begin(), v.get_binary().end())}});
        else
            row.push_back(v);
    }
    return Value({{"v", 1}, {"row", row}}).dump();
}
absl::StatusOr<std::vector<Value>> decodeRow(const std::string& payload)
{
    try
    {
        auto j = Value::parse(payload);
        if(j.at("v") != 1 || !j.at("row").is_array() || j.at("row").size() > 256)
            return absl::DataLossError("invalid row codec version or width");
        std::vector<Value> row;
        for(auto v: j.at("row"))
        {
            if(v.is_object())
            {
                if(v.size() != 1 || !v.contains("blob") || !v["blob"].is_array())
                    return absl::DataLossError("invalid BLOB codec");
                std::vector<uint8_t> bytes;
                for(const auto& b: v["blob"])
                {
                    if(!b.is_number_integer() || b.get<int64_t>() < 0 || b.get<int64_t>() > 255)
                        return absl::DataLossError("invalid BLOB byte");
                    bytes.push_back(b.get<uint8_t>());
                }
                v = Value::binary(bytes);
            }
            if(v.is_array() || (v.is_object() && !v.is_binary()))
                return absl::DataLossError("invalid typed row value");
            row.push_back(v);
        }
        return row;
    }
    catch(const std::exception& e)
    {
        return absl::DataLossError(e.what());
    }
}
} // namespace chronolog::sql
