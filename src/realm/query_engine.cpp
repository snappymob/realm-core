/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <realm/query_engine.hpp>

#include <realm/query_expression.hpp>
#include <realm/index_string.hpp>
#include <realm/db.hpp>
#include <realm/utilities.hpp>

using namespace realm;

ParentNode::ParentNode(const ParentNode& from)
    : m_child(from.m_child ? from.m_child->clone() : nullptr)
    , m_condition_column_name(from.m_condition_column_name)
    , m_condition_column_key(from.m_condition_column_key)
    , m_dD(from.m_dD)
    , m_dT(from.m_dT)
    , m_probes(from.m_probes)
    , m_matches(from.m_matches)
    , m_table(from.m_table)
{
}


size_t ParentNode::find_first(size_t start, size_t end)
{
    size_t sz = m_children.size();
    size_t current_cond = 0;
    size_t nb_cond_to_test = sz;

    while (REALM_LIKELY(start < end)) {
        size_t m = m_children[current_cond]->find_first_local(start, end);

        if (m != start) {
            // Pointer advanced - we will have to check all other conditions
            nb_cond_to_test = sz;
            start = m;
        }

        nb_cond_to_test--;

        // Optimized for one condition where this will be true first time
        if (REALM_LIKELY(nb_cond_to_test == 0))
            return m;

        current_cond++;

        if (current_cond == sz)
            current_cond = 0;
    }
    return not_found;
}

template <class T>
inline bool Obj::evaluate(T func) const
{
    Cluster cluster(0, get_alloc(), m_table->m_clusters);
    cluster.init(m_mem);
    cluster.set_offset(m_key.value - cluster.get_key_value(m_row_ndx));
    return func(&cluster, m_row_ndx);
}

bool ParentNode::match(const Obj& obj)
{
    return obj.evaluate([this](const Cluster* cluster, size_t row) {
        set_cluster(cluster);
        size_t m = find_first(row, row + 1);
        return m != npos;
    });
}

template <Action action>
void ParentNode::aggregate_local_prepare(DataType col_id, bool nullable)
{
    switch (col_id) {
        case type_Int: {
            if (nullable)
                m_column_action_specializer = &ThisType::column_action_specialization<action, ArrayIntNull>;
            else
                m_column_action_specializer = &ThisType::column_action_specialization<action, ArrayInteger>;
            break;
        }
        case type_Float:
            m_column_action_specializer = &ThisType::column_action_specialization<action, ArrayFloat>;
            break;
        case type_Double:
            m_column_action_specializer = &ThisType::column_action_specialization<action, ArrayDouble>;
            break;
        case type_Decimal:
            m_column_action_specializer = &ThisType::column_action_specialization<action, ArrayDecimal128>;
            break;
        default:
            REALM_ASSERT(false);
            break;
    }
}

void ParentNode::aggregate_local_prepare(Action TAction, DataType col_id, bool nullable)
{
    switch (TAction) {
        case act_ReturnFirst: {
            if (nullable)
                m_column_action_specializer = &ThisType::column_action_specialization<act_ReturnFirst, ArrayIntNull>;
            else
                m_column_action_specializer = &ThisType::column_action_specialization<act_ReturnFirst, ArrayInteger>;
            break;
        }
        case act_FindAll: {
            // For find_all(), the column below is a dummy and the caller sets it to nullptr. Hence, no data is being
            // read from any column upon each query match (just matchcount++ is performed), and we pass nullable =
            // false simply by convention. FIXME: Clean up all this.
            REALM_ASSERT(!nullable);
            m_column_action_specializer = &ThisType::column_action_specialization<act_FindAll, ArrayInteger>;
            break;
        }
        case act_Count: {
            // For count(), the column below is a dummy and the caller sets it to nullptr. Hence, no data is being
            // read from any column upon each query match (just matchcount++ is performed), and we pass nullable =
            // false simply by convention. FIXME: Clean up all this.
            REALM_ASSERT(!nullable);
            m_column_action_specializer = &ThisType::column_action_specialization<act_Count, ArrayInteger>;
            break;
        }
        case act_Sum: {
            aggregate_local_prepare<act_Sum>(col_id, nullable);
            break;
        }
        case act_Min: {
            aggregate_local_prepare<act_Min>(col_id, nullable);
            break;
        }
        case act_Max: {
            aggregate_local_prepare<act_Max>(col_id, nullable);
            break;
        }
        case act_CallbackIdx: {
            // Future features where for each query match, you want to perform an action that only requires knowlege
            // about the row index, and not the payload there. Examples could be find_all(), however, this code path
            // below is for new features given in a callback method and not yet supported by core.
            if (nullable)
                m_column_action_specializer = &ThisType::column_action_specialization<act_CallbackIdx, ArrayIntNull>;
            else
                m_column_action_specializer = &ThisType::column_action_specialization<act_CallbackIdx, ArrayInteger>;
            break;
        }
        default:
            REALM_ASSERT(false);
            break;
    }
}

size_t ParentNode::aggregate_local(QueryStateBase* st, size_t start, size_t end, size_t local_limit,
                                   ArrayPayload* source_column)
{
    // aggregate called on non-integer column type. Speed of this function is not as critical as speed of the
    // integer version, because find_first_local() is relatively slower here (because it's non-integers).
    //
    // Todo: Two speedups are possible. Simple: Initially test if there are no sub criterias and run
    // find_first_local()
    // in a tight loop if so (instead of testing if there are sub criterias after each match). Harder: Specialize
    // data type array to make array call match() directly on each match, like for integers.

    m_state = st;
    size_t local_matches = 0;

    size_t r = start - 1;
    for (;;) {
        if (local_matches == local_limit) {
            m_dD = double(r - start) / (local_matches + 1.1);
            return r + 1;
        }

        // Find first match in this condition node
        r = find_first_local(r + 1, end);
        if (r == not_found) {
            m_dD = double(r - start) / (local_matches + 1.1);
            return end;
        }

        local_matches++;

        // Find first match in remaining condition nodes
        size_t m = r;

        for (size_t c = 1; c < m_children.size(); c++) {
            m = m_children[c]->find_first_local(r, r + 1);
            if (m != r) {
                break;
            }
        }

        // If index of first match in this node equals index of first match in all remaining nodes, we have a final
        // match
        if (m == r) {
            bool cont = (this->*m_column_action_specializer)(st, source_column, r);
            if (!cont) {
                return static_cast<size_t>(-1);
            }
        }
    }
}

void StringNodeEqualBase::init(bool will_query_ranges)
{
    m_dD = 10.0;
    StringNodeBase::init(will_query_ranges);

    if (m_is_string_enum) {
        m_dT = 1.0;
    }
    else if (m_has_search_index) {
        m_dT = 0.0;
    }
    else {
        m_dT = 10.0;
    }

    if (m_has_search_index) {
        // Will set m_index_matches, m_index_matches_destroy, m_results_start and m_results_end
        _search_index_init();
    }
}

size_t StringNodeEqualBase::find_first_local(size_t start, size_t end)
{
    REALM_ASSERT(m_table);

    if (m_has_search_index) {
        if (start < end) {
            ObjKey first_key = m_cluster->get_real_key(start);
            if (first_key < m_last_start_key) {
                // We are not advancing through the clusters. We basically don't know where we are,
                // so just start over from the beginning.
                m_results_ndx = m_results_start;
                m_actual_key = get_key(m_results_ndx);
            }
            m_last_start_key = first_key;

            // Check if we can expect to find more keys
            if (m_results_ndx < m_results_end) {
                // Check if we should advance to next key to search for
                while (first_key > m_actual_key) {
                    m_results_ndx++;
                    if (m_results_ndx == m_results_end) {
                        return not_found;
                    }
                    m_actual_key = get_key(m_results_ndx);
                }

                // If actual_key is bigger than last key, it is not in this leaf
                ObjKey last_key = m_cluster->get_real_key(end - 1);
                if (m_actual_key > last_key)
                    return not_found;

                // Now actual_key must be found in leaf keys
                return m_cluster->lower_bound_key(ObjKey(m_actual_key.value - m_cluster->get_offset()));
            }
        }
        return not_found;
    }

    return _find_first_local(start, end);
}


namespace realm {

size_t do_search_index(ObjKey& last_start_key, size_t& result_get, std::vector<ObjKey>& results,
                       const Cluster* cluster, size_t start, size_t end)
{
    ObjKey first_key = cluster->get_real_key(start);
    if (first_key < last_start_key) {
        // We are not advancing through the clusters. We basically don't know where we are,
        // so just start over from the beginning.
        auto it = std::lower_bound(results.begin(), results.end(), first_key);
        result_get = (it == results.end()) ? realm::npos : (it - results.begin());
    }
    last_start_key = first_key;

    if (result_get < results.size()) {
        auto actual_key = results[result_get];
        // skip through keys which are in "earlier" leafs than the one selected by start..end:
        while (first_key > actual_key) {
            result_get++;
            if (result_get == results.size())
                return not_found;
            actual_key = results[result_get];
        }

        // if actual key is bigger than last key, it is not in this leaf
        ObjKey last_key = cluster->get_real_key(end - 1);
        if (actual_key > last_key)
            return not_found;

        // key is known to be in this leaf, so find key whithin leaf keys
        return cluster->lower_bound_key(ObjKey(actual_key.value - cluster->get_offset()));
    }
    return not_found;
}

void StringNode<Equal>::_search_index_init()
{
    FindRes fr;
    InternalFindResult res;

    m_last_start_key = ObjKey();
    m_results_start = 0;
    if (ParentNode::m_table->get_primary_key_column() == ParentNode::m_condition_column_key) {
        m_actual_key = ParentNode::m_table.unchecked_ptr()->find_first(ParentNode::m_condition_column_key,
                                                                       StringData(StringNodeBase::m_value));
        m_results_end = m_actual_key ? 1 : 0;
    }
    else {
        auto index = ParentNode::m_table.unchecked_ptr()->get_search_index(ParentNode::m_condition_column_key);
        fr = index->find_all_no_copy(StringData(StringNodeBase::m_value), res);

        m_index_matches.reset();
        switch (fr) {
            case FindRes_single:
                m_actual_key = ObjKey(res.payload);
                m_results_end = 1;
                break;
            case FindRes_column:
                m_index_matches.reset(
                    new IntegerColumn(m_table.unchecked_ptr()->get_alloc(), ref_type(res.payload))); // Throws
                m_results_start = res.start_ndx;
                m_results_end = res.end_ndx;
                m_actual_key = ObjKey(m_index_matches->get(m_results_start));
                break;
            case FindRes_not_found:
                m_results_end = 0;
                break;
        }
    }
    m_results_ndx = m_results_start;
}

bool StringNode<Equal>::do_consume_condition(ParentNode& node)
{
    // Don't use the search index if present since we're in a scenario where
    // it'd be slower
    m_has_search_index = false;

    auto& other = static_cast<StringNode<Equal>&>(node);
    REALM_ASSERT(m_condition_column_key == other.m_condition_column_key);
    REALM_ASSERT(other.m_needles.empty());
    if (m_needles.empty()) {
        m_needles.insert(m_value ? StringData(*m_value) : StringData());
    }
    if (auto& str = other.m_value) {
        m_needle_storage.push_back(std::make_unique<char[]>(str->size()));
        std::copy(str->data(), str->data() + str->size(), m_needle_storage.back().get());
        m_needles.insert(StringData(m_needle_storage.back().get(), str->size()));
    }
    else {
        m_needles.emplace();
    }
    return true;
}

size_t StringNode<Equal>::_find_first_local(size_t start, size_t end)
{
    if (m_needles.empty()) {
        return m_leaf_ptr->find_first(m_value, start, end);
    }
    else {
        if (end == npos)
            end = m_leaf_ptr->size();
        REALM_ASSERT_3(start, <=, end);
        return find_first_haystack<20>(*m_leaf_ptr, m_needles, start, end);
    }
}

std::string StringNode<Equal>::describe(util::serializer::SerialisationState& state) const
{
    if (m_needles.empty()) {
        return StringNodeEqualBase::describe(state);
    }

    // FIXME: once the parser supports it, print something like "column IN {s1, s2, s3}"
    std::string desc;
    bool is_first = true;
    for (auto it : m_needles) {
        StringData sd(it.data(), it.size());
        desc += (is_first ? "" : " or ") + state.describe_column(ParentNode::m_table, m_condition_column_key) + " " +
                Equal::description() + " " + util::serializer::print_value(sd);
        is_first = false;
    }
    if (!is_first) {
        desc = "(" + desc + ")";
    }
    return desc;
}


void StringNode<EqualIns>::_search_index_init()
{
    auto index = ParentNode::m_table->get_search_index(ParentNode::m_condition_column_key);
    m_index_matches.clear();
    index->find_all(m_index_matches, StringData(StringNodeBase::m_value), true);
    m_results_start = 0;
    m_results_ndx = 0;
    m_results_end = m_index_matches.size();
    if (m_results_start != m_results_end) {
        m_actual_key = m_index_matches[0];
    }
}

size_t StringNode<EqualIns>::_find_first_local(size_t start, size_t end)
{
    EqualIns cond;
    for (size_t s = start; s < end; ++s) {
        StringData t = get_string(s);

        if (cond(StringData(m_value), m_ucase.c_str(), m_lcase.c_str(), t))
            return s;
    }

    return not_found;
}

std::unique_ptr<ArrayPayload> TwoColumnsNodeBase::update_cached_leaf_pointers_for_column(Allocator& alloc,
                                                                                         const ColKey& col_key)
{
    switch (col_key.get_type()) {
        case col_type_Int:
            if (col_key.is_nullable()) {
                return std::make_unique<ArrayIntNull>(alloc);
            }
            return std::make_unique<ArrayInteger>(alloc);
        case col_type_Bool:
            return std::make_unique<ArrayBoolNull>(alloc);
        case col_type_String:
            return std::make_unique<ArrayString>(alloc);
        case col_type_Binary:
            return std::make_unique<ArrayBinary>(alloc);
        case col_type_Mixed:
            return std::make_unique<ArrayMixed>(alloc);
        case col_type_Timestamp:
            return std::make_unique<ArrayTimestamp>(alloc);
        case col_type_Float:
            return std::make_unique<ArrayFloatNull>(alloc);
        case col_type_Double:
            return std::make_unique<ArrayDoubleNull>(alloc);
        case col_type_Decimal:
            return std::make_unique<ArrayDecimal128>(alloc);
        case col_type_Link:
            return std::make_unique<ArrayKey>(alloc);
        case col_type_ObjectId:
            return std::make_unique<ArrayObjectIdNull>(alloc);
        case col_type_UUID:
            return std::make_unique<ArrayUUIDNull>(alloc);
        case col_type_TypedLink:
        case col_type_BackLink:
        case col_type_LinkList:
        case col_type_OldDateTime:
        case col_type_OldTable:
            break;
    };
    REALM_UNREACHABLE();
    return {};
}

Mixed TwoColumnsNodeBase::get_value_from_leaf(ArrayPayload* leaf, ColumnType col_type, bool nullable, size_t ndx)
{
    switch (col_type) {
        case col_type_Int:
            if (nullable) {
                return (static_cast<ArrayIntNull*>(leaf))->get(ndx);
            }
            return (static_cast<ArrayInteger*>(leaf))->get(ndx);
        case col_type_Bool:
            return (static_cast<ArrayBoolNull*>(leaf))->get(ndx);
        case col_type_String:
            return (static_cast<ArrayString*>(leaf))->get(ndx);
        case col_type_Binary:
            return (static_cast<ArrayBinary*>(leaf))->get(ndx);
        case col_type_Mixed:
            return (static_cast<ArrayMixed*>(leaf))->get(ndx);
        case col_type_Timestamp:
            return (static_cast<ArrayTimestamp*>(leaf))->get(ndx);
        case col_type_Float:
            return (static_cast<ArrayFloatNull*>(leaf))->get(ndx);
        case col_type_Double:
            return (static_cast<ArrayDoubleNull*>(leaf))->get(ndx);
        case col_type_Decimal:
            return (static_cast<ArrayDecimal128*>(leaf))->get(ndx);
        case col_type_Link:
            return (static_cast<ArrayKey*>(leaf))->get(ndx);
        case col_type_ObjectId:
            return (static_cast<ArrayObjectIdNull*>(leaf))->get(ndx);
        case col_type_UUID:
            return (static_cast<ArrayUUIDNull*>(leaf))->get(ndx);
        case col_type_TypedLink:
        case col_type_BackLink:
        case col_type_LinkList:
        case col_type_OldDateTime:
        case col_type_OldTable:
            break;
    };
    REALM_UNREACHABLE();
    return {};
}


} // namespace realm

size_t NotNode::find_first_local(size_t start, size_t end)
{
    if (start <= m_known_range_start && end >= m_known_range_end) {
        return find_first_covers_known(start, end);
    }
    else if (start >= m_known_range_start && end <= m_known_range_end) {
        return find_first_covered_by_known(start, end);
    }
    else if (start < m_known_range_start && end >= m_known_range_start) {
        return find_first_overlap_lower(start, end);
    }
    else if (start <= m_known_range_end && end > m_known_range_end) {
        return find_first_overlap_upper(start, end);
    }
    else { // start > m_known_range_end || end < m_known_range_start
        return find_first_no_overlap(start, end);
    }
}

bool NotNode::evaluate_at(size_t rowndx)
{
    return m_condition->find_first(rowndx, rowndx + 1) == not_found;
}

void NotNode::update_known(size_t start, size_t end, size_t first)
{
    m_known_range_start = start;
    m_known_range_end = end;
    m_first_in_known_range = first;
}

size_t NotNode::find_first_loop(size_t start, size_t end)
{
    for (size_t i = start; i < end; ++i) {
        if (evaluate_at(i)) {
            return i;
        }
    }
    return not_found;
}

size_t NotNode::find_first_covers_known(size_t start, size_t end)
{
    // CASE: start-end covers the known range
    // [    ######    ]
    REALM_ASSERT_DEBUG(start <= m_known_range_start && end >= m_known_range_end);
    size_t result = find_first_loop(start, m_known_range_start);
    if (result != not_found) {
        update_known(start, m_known_range_end, result);
    }
    else {
        if (m_first_in_known_range != not_found) {
            update_known(start, m_known_range_end, m_first_in_known_range);
            result = m_first_in_known_range;
        }
        else {
            result = find_first_loop(m_known_range_end, end);
            update_known(start, end, result);
        }
    }
    return result;
}

size_t NotNode::find_first_covered_by_known(size_t start, size_t end)
{
    REALM_ASSERT_DEBUG(start >= m_known_range_start && end <= m_known_range_end);
    // CASE: the known range covers start-end
    // ###[#####]###
    if (m_first_in_known_range != not_found) {
        if (m_first_in_known_range > end) {
            return not_found;
        }
        else if (m_first_in_known_range >= start) {
            return m_first_in_known_range;
        }
    }
    // The first known match is before start, so we can't use the results to improve
    // heuristics.
    return find_first_loop(start, end);
}

size_t NotNode::find_first_overlap_lower(size_t start, size_t end)
{
    REALM_ASSERT_DEBUG(start < m_known_range_start && end >= m_known_range_start && end <= m_known_range_end);
    static_cast<void>(end);
    // CASE: partial overlap, lower end
    // [   ###]#####
    size_t result;
    result = find_first_loop(start, m_known_range_start);
    if (result == not_found) {
        result = m_first_in_known_range;
    }
    update_known(start, m_known_range_end, result);
    return result < end ? result : not_found;
}

size_t NotNode::find_first_overlap_upper(size_t start, size_t end)
{
    REALM_ASSERT_DEBUG(start <= m_known_range_end && start >= m_known_range_start && end > m_known_range_end);
    // CASE: partial overlap, upper end
    // ####[###    ]
    size_t result;
    if (m_first_in_known_range != not_found) {
        if (m_first_in_known_range >= start) {
            result = m_first_in_known_range;
            update_known(m_known_range_start, end, result);
        }
        else {
            result = find_first_loop(start, end);
            update_known(m_known_range_start, end, m_first_in_known_range);
        }
    }
    else {
        result = find_first_loop(m_known_range_end, end);
        update_known(m_known_range_start, end, result);
    }
    return result;
}

size_t NotNode::find_first_no_overlap(size_t start, size_t end)
{
    REALM_ASSERT_DEBUG((start < m_known_range_start && end < m_known_range_start) ||
                       (start > m_known_range_end && end > m_known_range_end));
    // CASE: no overlap
    // ### [    ]   or    [    ] ####
    // if input is a larger range, discard and replace with results.
    size_t result = find_first_loop(start, end);
    if (end - start > m_known_range_end - m_known_range_start) {
        update_known(start, end, result);
    }
    return result;
}

ExpressionNode::ExpressionNode(std::unique_ptr<Expression> expression)
: m_expression(std::move(expression))
{
    m_dD = 100.0;
    m_dT = 50.0;
}

void ExpressionNode::table_changed()
{
    m_expression->set_base_table(m_table);
}

void ExpressionNode::cluster_changed()
{
    m_expression->set_cluster(m_cluster);
}

void ExpressionNode::init(bool will_query_ranges)
{
    ParentNode::init(will_query_ranges);
    m_dT = m_expression->init();
}

std::string ExpressionNode::describe(util::serializer::SerialisationState& state) const
{
    if (m_expression) {
        return m_expression->description(state);
    }
    else {
        return "empty expression";
    }
}

void ExpressionNode::collect_dependencies(std::vector<TableKey>& tables) const
{
    m_expression->collect_dependencies(tables);
}

size_t ExpressionNode::find_first_local(size_t start, size_t end)
{
    return m_expression->find_first(start, end);
}

std::unique_ptr<ParentNode> ExpressionNode::clone() const
{
    return std::unique_ptr<ParentNode>(new ExpressionNode(*this));
}

ExpressionNode::ExpressionNode(const ExpressionNode& from)
    : ParentNode(from)
    , m_expression(from.m_expression->clone())
{
}
