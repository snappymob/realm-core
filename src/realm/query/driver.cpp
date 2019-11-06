#include "realm/query/driver.hpp"
#include "realm/query/query_bison.hpp"
#include <realm/parser/query_builder.hpp>

using namespace realm;

int ParserDriver::parse(const std::string& str)
{
    parse_string = str;
    std::string dummy;
    location.initialize(&dummy);
    scan_begin();
    yy::parser parse(*this);
    parse.set_debug_level(trace_parsing);
    int res = parse();
    scan_end();
    if (parse_error) {
        throw std::runtime_error(error_string);
    }
    return res;
}

Subexpr* LinkChain::column(std::string col)
{
    auto col_key = m_current_table->get_column_key(col);
    if (!col_key) {
        std::string err = m_current_table->get_name();
        err += " has no property: ";
        err += col;
        throw std::runtime_error(err);
    }

    if (m_current_table->is_list(col_key)) {
        switch (col_key.get_type()) {
        case col_type_Int:
            return new Columns<Lst<Int>>(col_key, m_base_table, m_link_cols);
        case col_type_String:
            return new Columns<Lst<String>>(col_key, m_base_table, m_link_cols);
        default:
            break;
        }
    }
    else {
        switch (col_key.get_type()) {
        case col_type_Int:
            return new Columns<Int>(col_key, m_base_table, m_link_cols);
        case col_type_String:
            return new Columns<String>(col_key, m_base_table, m_link_cols);
        default:
            break;
        }
    }
    return nullptr;
}
namespace {

class MixedArguments : public query_builder::Arguments {
public:
    MixedArguments(const std::vector<Mixed>& args)
        : m_args(args)
    {
    }
    bool bool_for_argument(size_t n) final
    {
        return m_args.at(n).get<bool>();
    }
    long long long_for_argument(size_t n) final
    {
        return m_args.at(n).get<int64_t>();
    }
    float float_for_argument(size_t n) final
    {
        return m_args.at(n).get<float>();
    }
    double double_for_argument(size_t n) final
    {
        return m_args.at(n).get<double>();
    }
    StringData string_for_argument(size_t n) final
    {
        return m_args.at(n).get<StringData>();
    }
    BinaryData binary_for_argument(size_t n) final
    {
        return m_args.at(n).get<BinaryData>();
    }
    Timestamp timestamp_for_argument(size_t n) final
    {
        return m_args.at(n).get<Timestamp>();
    }
    ObjectId objectid_for_argument(size_t n) final
    {
        return m_args.at(n).get<ObjectId>();
    }
    UUID uuid_for_argument(size_t n) final
    {
        return m_args.at(n).get<UUID>();
    }
    Decimal128 decimal128_for_argument(size_t n) final
    {
        return m_args.at(n).get<Decimal128>();
    }
    ObjKey object_index_for_argument(size_t n) final
    {
        return m_args.at(n).get<ObjKey>();
    }
    bool is_argument_null(size_t n) final
    {
        return m_args.at(n).is_null();
    }

private:
    const std::vector<Mixed>& m_args;
};

} // namespace

Query Table::query(const std::string& query_string, const std::vector<Mixed>& arguments) const
{
    MixedArguments args(arguments);
    return query(query_string, args, {});
}

Query Table::query(const std::string& query_string, query_builder::Arguments&,
                   const parser::KeyPathMapping&) const
{
    ParserDriver driver(m_own_ref);
    driver.parse(query_string);
    return std::move(driver.result);
}

