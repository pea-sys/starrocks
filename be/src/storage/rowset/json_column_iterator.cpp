// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "storage/rowset/json_column_iterator.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "column/column_access_path.h"
#include "column/column_helper.h"
#include "column/const_column.h"
#include "column/json_column.h"
#include "column/nullable_column.h"
#include "column/struct_column.h"
#include "column/vectorized_fwd.h"
#include "common/object_pool.h"
#include "common/status.h"
#include "common/statusor.h"
#include "exprs/cast_expr.h"
#include "exprs/column_ref.h"
#include "exprs/expr_context.h"
#include "gutil/casts.h"
#include "runtime/descriptors.h"
#include "runtime/types.h"
#include "storage/rowset/column_iterator.h"
#include "storage/rowset/column_reader.h"
#include "storage/rowset/scalar_column_iterator.h"
#include "types/logical_type.h"
#include "util/json_flattener.h"
#include "util/runtime_profile.h"

namespace starrocks {

class JsonFlatColumnIterator final : public ColumnIterator {
public:
    JsonFlatColumnIterator(ColumnReader* _reader, std::unique_ptr<ColumnIterator>& null_iter,
                           std::vector<std::unique_ptr<ColumnIterator>>& field_iters,
                           std::vector<std::string>& flat_paths, std::vector<LogicalType> target_types,
                           std::vector<LogicalType> source_types, ColumnAccessPath* path)
            : _reader(_reader),
              _null_iter(std::move(null_iter)),
              _flat_iters(std::move(field_iters)),
              _flat_paths(std::move(flat_paths)),
              _target_types(std::move(target_types)),
              _source_types(std::move(source_types)),
              _path(path) {}

    ~JsonFlatColumnIterator() override = default;

    Status init(const ColumnIteratorOptions& opts) override;

    Status next_batch(size_t* n, Column* dst) override;

    Status next_batch(const SparseRange<>& range, Column* dst) override;

    Status seek_to_first() override;

    Status seek_to_ordinal(ordinal_t ord) override;

    ordinal_t get_current_ordinal() const override { return _flat_iters[0]->get_current_ordinal(); }

    ordinal_t num_rows() const override { return _flat_iters[0]->num_rows(); }

    Status get_row_ranges_by_zone_map(const std::vector<const ColumnPredicate*>& predicates,
                                      const ColumnPredicate* del_predicate, SparseRange<>* row_ranges,
                                      CompoundNodeType pred_relation) override;

    Status fetch_values_by_rowid(const rowid_t* rowids, size_t size, Column* values) override;

private:
    template <typename FUNC>
    Status _read_and_cast(JsonColumn* json_column, FUNC fn);

private:
    ColumnReader* _reader;

    std::unique_ptr<ColumnIterator> _null_iter;
    std::vector<std::unique_ptr<ColumnIterator>> _flat_iters;
    std::vector<std::string> _flat_paths;
    std::vector<LogicalType> _target_types;
    std::vector<LogicalType> _source_types;
    // to avoid create column with find type
    std::vector<ColumnPtr> _source_columns;
    ColumnAccessPath* _path;

    ObjectPool _pool;
    std::vector<Expr*> _cast_exprs;
};

Status JsonFlatColumnIterator::init(const ColumnIteratorOptions& opts) {
    RETURN_IF_ERROR(ColumnIterator::init(opts));
    if (_null_iter != nullptr) {
        RETURN_IF_ERROR(_null_iter->init(opts));
    }

    for (auto& iter : _flat_iters) {
        RETURN_IF_ERROR(iter->init(opts));
    }

    // update stats
    DCHECK(_path != nullptr);
    auto abs_path = _path->absolute_path();
    if (opts.stats->flat_json_hits.count(abs_path) == 0) {
        opts.stats->flat_json_hits[abs_path] = 1;
    } else {
        opts.stats->flat_json_hits[abs_path] = opts.stats->flat_json_hits[abs_path] + 1;
    }

    DCHECK(_target_types.size() == _source_types.size());

    for (int i = 0; i < _target_types.size(); i++) {
        if (_target_types[i] == _source_types[i]) {
            _cast_exprs.push_back(nullptr);
            _source_columns.push_back(nullptr);
            continue;
        }

        TypeDescriptor source_type(_source_types[i]);
        TypeDescriptor target_type(_target_types[i]);

        SlotDescriptor source_slot(i, "mock_solt", source_type);
        ColumnRef* col_ref = _pool.add(new ColumnRef(&source_slot));

        auto cast_expr = VectorizedCastExprFactory::from_type(source_type, target_type, col_ref, &_pool);
        _cast_exprs.push_back(cast_expr);
        _source_columns.push_back(ColumnHelper::create_column(TypeDescriptor(_source_types[i]), true));
    }

    return Status::OK();
}

template <typename FUNC>
Status JsonFlatColumnIterator::_read_and_cast(JsonColumn* json_column, FUNC read_fn) {
    json_column->init_flat_columns(_flat_paths, _target_types);
    Chunk chunk;

    for (int i = 0; i < _flat_iters.size(); i++) {
        if (_cast_exprs[i] != nullptr) {
            ColumnPtr source = _source_columns[i]->clone_empty();
            RETURN_IF_ERROR(read_fn(i, source.get()));

            chunk.append_column(source, i);
            ASSIGN_OR_RETURN(auto res, _cast_exprs[i]->evaluate_checked(nullptr, &chunk));
            auto target = json_column->get_flat_field(i);
            target->set_delete_state(source->delete_state());
            if (res->only_null()) {
                target->append_nulls(source->size());
            } else if (res->is_constant()) {
                auto data = down_cast<ConstColumn*>(res.get())->data_column();
                target->append_value_multiple_times(*data, 0, source->size());
            } else {
                target->append(*res, 0, source->size());
            }
            DCHECK_EQ(json_column->size(), target->size());
        } else {
            auto* flat_column = json_column->get_flat_field(i).get();
            RETURN_IF_ERROR(read_fn(i, flat_column));
        }
    }
    return Status::OK();
}

Status JsonFlatColumnIterator::next_batch(size_t* n, Column* dst) {
    JsonColumn* json_column = nullptr;
    NullColumn* null_column = nullptr;
    if (dst->is_nullable()) {
        auto* nullable_column = down_cast<NullableColumn*>(dst);

        json_column = down_cast<JsonColumn*>(nullable_column->data_column().get());
        null_column = down_cast<NullColumn*>(nullable_column->null_column().get());
    } else {
        json_column = down_cast<JsonColumn*>(dst);
    }

    // 1. Read null column
    if (_null_iter != nullptr) {
        RETURN_IF_ERROR(_null_iter->next_batch(n, null_column));
        down_cast<NullableColumn*>(dst)->update_has_null();
    }

    // 2. Read flat column
    auto read = [&](int index, Column* column) { return _flat_iters[index]->next_batch(n, column); };
    return _read_and_cast(json_column, read);
}

Status JsonFlatColumnIterator::next_batch(const SparseRange<>& range, Column* dst) {
    JsonColumn* json_column = nullptr;
    NullColumn* null_column = nullptr;
    if (dst->is_nullable()) {
        auto* nullable_column = down_cast<NullableColumn*>(dst);
        json_column = down_cast<JsonColumn*>(nullable_column->data_column().get());
        null_column = down_cast<NullColumn*>(nullable_column->null_column().get());
    } else {
        json_column = down_cast<JsonColumn*>(dst);
    }

    CHECK((_null_iter == nullptr && null_column == nullptr) || (_null_iter != nullptr && null_column != nullptr));

    // 1. Read null column
    if (_null_iter != nullptr) {
        RETURN_IF_ERROR(_null_iter->next_batch(range, null_column));
        down_cast<NullableColumn*>(dst)->update_has_null();
    }

    // 2. Read flat column
    auto read = [&](int index, Column* column) { return _flat_iters[index]->next_batch(range, column); };
    return _read_and_cast(json_column, read);
}

Status JsonFlatColumnIterator::fetch_values_by_rowid(const rowid_t* rowids, size_t size, Column* values) {
    JsonColumn* json_column = nullptr;
    NullColumn* null_column = nullptr;
    // 1. Read null column
    if (_null_iter != nullptr) {
        auto* nullable_column = down_cast<NullableColumn*>(values);
        json_column = down_cast<JsonColumn*>(nullable_column->data_column().get());
        null_column = down_cast<NullColumn*>(nullable_column->null_column().get());
        RETURN_IF_ERROR(_null_iter->fetch_values_by_rowid(rowids, size, null_column));
        nullable_column->update_has_null();
    } else {
        json_column = down_cast<JsonColumn*>(values);
    }

    // 2. Read flat column
    auto read = [&](int index, Column* column) {
        return _flat_iters[index]->fetch_values_by_rowid(rowids, size, column);
    };

    return _read_and_cast(json_column, read);
}

Status JsonFlatColumnIterator::seek_to_first() {
    if (_null_iter != nullptr) {
        RETURN_IF_ERROR(_null_iter->seek_to_first());
    }
    for (int i = 0; i < _flat_iters.size(); i++) {
        RETURN_IF_ERROR(_flat_iters[i]->seek_to_first());
    }
    return Status::OK();
}

Status JsonFlatColumnIterator::seek_to_ordinal(ordinal_t ord) {
    if (_null_iter != nullptr) {
        RETURN_IF_ERROR(_null_iter->seek_to_ordinal(ord));
    }
    for (int i = 0; i < _flat_iters.size(); i++) {
        RETURN_IF_ERROR(_flat_iters[i]->seek_to_ordinal(ord));
    }
    return Status::OK();
}

Status JsonFlatColumnIterator::get_row_ranges_by_zone_map(const std::vector<const ColumnPredicate*>& predicates,
                                                          const ColumnPredicate* del_predicate,
                                                          SparseRange<>* row_ranges, CompoundNodeType pred_relation) {
    row_ranges->add({0, static_cast<rowid_t>(_reader->num_rows())});
    return Status::OK();
}

class JsonDynamicFlatIterator final : public ColumnIterator {
public:
    JsonDynamicFlatIterator(std::unique_ptr<ScalarColumnIterator>& json_iter, std::vector<std::string> flat_paths,
                            std::vector<LogicalType> target_types, ColumnAccessPath* path)
            : _json_iter(std::move(json_iter)),
              _flat_paths(std::move(flat_paths)),
              _target_types(std::move(target_types)),
              _path(path){};

    ~JsonDynamicFlatIterator() override = default;

    Status init(const ColumnIteratorOptions& opts) override;

    Status next_batch(size_t* n, Column* dst) override;

    Status next_batch(const SparseRange<>& range, Column* dst) override;

    Status seek_to_first() override;

    Status seek_to_ordinal(ordinal_t ord) override;

    ordinal_t get_current_ordinal() const override { return _json_iter->get_current_ordinal(); }

    ordinal_t num_rows() const override { return _json_iter->num_rows(); }

    /// for vectorized engine
    Status get_row_ranges_by_zone_map(const std::vector<const ColumnPredicate*>& predicates,
                                      const ColumnPredicate* del_predicate, SparseRange<>* row_ranges,
                                      CompoundNodeType pred_relation) override;

    Status fetch_values_by_rowid(const rowid_t* rowids, size_t size, Column* values) override;

private:
    Status _flat_json(Column* input, Column* output);

private:
    std::unique_ptr<ScalarColumnIterator> _json_iter;
    std::vector<std::string> _flat_paths;
    std::vector<LogicalType> _target_types;
    ColumnAccessPath* _path;

    JsonFlattener _flattener;
};

Status JsonDynamicFlatIterator::init(const ColumnIteratorOptions& opts) {
    RETURN_IF_ERROR(ColumnIterator::init(opts));
    DCHECK(_path != nullptr);
    auto abs_path = _path->absolute_path();
    if (opts.stats->dynamic_json_hits.count(abs_path) == 0) {
        opts.stats->dynamic_json_hits[abs_path] = 1;
    } else {
        opts.stats->dynamic_json_hits[abs_path] = opts.stats->dynamic_json_hits[abs_path] + 1;
    }

    _flattener = JsonFlattener(_flat_paths, _target_types);
    return _json_iter->init(opts);
}

Status JsonDynamicFlatIterator::_flat_json(Column* input, Column* output) {
    SCOPED_RAW_TIMER(&_opts.stats->json_flatten_ns);
    JsonColumn* json_data = nullptr;

    // 1. null column handle
    if (output->is_nullable()) {
        // append null column
        auto* output_nullable = down_cast<NullableColumn*>(output);
        auto* output_null = down_cast<NullColumn*>(output_nullable->null_column().get());

        auto* input_nullable = down_cast<NullableColumn*>(input);
        auto* input_null = down_cast<NullColumn*>(input_nullable->null_column().get());

        output_null->append(*input_null, 0, input_null->size());
        output_nullable->set_has_null(input_nullable->has_null() | output_nullable->has_null());

        // json column
        json_data = down_cast<JsonColumn*>(output_nullable->data_column().get());
    } else {
        json_data = down_cast<JsonColumn*>(output);
    }

    // 2. flat
    json_data->init_flat_columns(_flat_paths, _target_types);
    _flattener.flatten(input, &(json_data->get_flat_fields()));
    return Status::OK();
}

Status JsonDynamicFlatIterator::next_batch(size_t* n, Column* dst) {
    auto proxy = dst->clone_empty();
    RETURN_IF_ERROR(_json_iter->next_batch(n, proxy.get()));
    dst->set_delete_state(proxy->delete_state());
    return _flat_json(proxy.get(), dst);
}

Status JsonDynamicFlatIterator::next_batch(const SparseRange<>& range, Column* dst) {
    auto proxy = dst->clone_empty();
    RETURN_IF_ERROR(_json_iter->next_batch(range, proxy.get()));
    dst->set_delete_state(proxy->delete_state());
    return _flat_json(proxy.get(), dst);
}

Status JsonDynamicFlatIterator::fetch_values_by_rowid(const rowid_t* rowids, size_t size, Column* values) {
    auto proxy = values->clone_empty();
    RETURN_IF_ERROR(_json_iter->fetch_values_by_rowid(rowids, size, proxy.get()));
    values->set_delete_state(proxy->delete_state());
    return _flat_json(proxy.get(), values);
}

Status JsonDynamicFlatIterator::seek_to_first() {
    return _json_iter->seek_to_first();
}

Status JsonDynamicFlatIterator::seek_to_ordinal(ordinal_t ord) {
    return _json_iter->seek_to_ordinal(ord);
}

Status JsonDynamicFlatIterator::get_row_ranges_by_zone_map(const std::vector<const ColumnPredicate*>& predicates,
                                                           const ColumnPredicate* del_predicate,
                                                           SparseRange<>* row_ranges, CompoundNodeType pred_relation) {
    return _json_iter->get_row_ranges_by_zone_map(predicates, del_predicate, row_ranges, pred_relation);
}

StatusOr<std::unique_ptr<ColumnIterator>> create_json_flat_iterator(
        ColumnReader* reader, std::unique_ptr<ColumnIterator> null_iter,
        std::vector<std::unique_ptr<ColumnIterator>> field_iters, std::vector<std::string>& full_paths,
        std::vector<LogicalType>& target_types, std::vector<LogicalType>& source_types, ColumnAccessPath* path) {
    return std::make_unique<JsonFlatColumnIterator>(reader, null_iter, field_iters, full_paths, target_types,
                                                    source_types, path);
}

StatusOr<std::unique_ptr<ColumnIterator>> create_json_dynamic_flat_iterator(
        std::unique_ptr<ScalarColumnIterator> json_iter, std::vector<std::string>& flat_paths,
        std::vector<LogicalType>& target_types, ColumnAccessPath* path) {
    return std::make_unique<JsonDynamicFlatIterator>(json_iter, flat_paths, target_types, path);
}

} // namespace starrocks
