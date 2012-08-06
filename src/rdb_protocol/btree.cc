#include <string>
#include <vector>

#include "errors.hpp"
#include <boost/shared_ptr.hpp>
#include <boost/variant.hpp>

#include "btree/backfill.hpp"
#include "btree/depth_first_traversal.hpp"
#include "btree/erase_range.hpp"
#include "containers/archive/vector_stream.hpp"
#include "containers/scoped.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/environment.hpp"
#include "rdb_protocol/query_language.hpp"

boost::shared_ptr<scoped_cJSON_t> get_data(const rdb_value_t *value, transaction_t *txn) {
    blob_t blob(const_cast<rdb_value_t *>(value)->value_ref(), blob::btree_maxreflen);

    boost::shared_ptr<scoped_cJSON_t> data;

    /* Grab the data from the blob. */
    //TODO unnecessary copies, I hate them
    std::string serialized_data = blob.read_to_string(txn, 0, blob.valuesize());

    /* Deserialize the value and return it. */
    std::vector<char> data_vec(serialized_data.begin(), serialized_data.end());

    vector_read_stream_t read_stream(&data_vec);

    int res = deserialize(&read_stream, &data);
    guarantee_err(res == 0, "corruption detected... this should probably be an exception\n");

    return data;
}

bool btree_value_fits(block_size_t bs, int data_length, const rdb_value_t *value) {
    return blob::ref_fits(bs, data_length, value->value_ref(), blob::btree_maxreflen);
}

point_read_response_t rdb_get(const store_key_t &store_key, btree_slice_t *slice, transaction_t *txn, superblock_t *superblock) {
    keyvalue_location_t<rdb_value_t> kv_location;
    find_keyvalue_location_for_read(txn, superblock, store_key.btree_key(), &kv_location, slice->root_eviction_priority, &slice->stats);

    if (!kv_location.value.has()) {
        return point_read_response_t();
    }

    boost::shared_ptr<scoped_cJSON_t> data = get_data(kv_location.value.get(), txn);

    return point_read_response_t(data);
}

point_write_response_t rdb_set(const store_key_t &key, boost::shared_ptr<scoped_cJSON_t> data,
                       btree_slice_t *slice, repli_timestamp_t timestamp,
                       transaction_t *txn, superblock_t *superblock) {
    //block_size_t block_size = slice->cache()->get_block_size();

    keyvalue_location_t<rdb_value_t> kv_location;
    find_keyvalue_location_for_write(txn, superblock, key.btree_key(), &kv_location, &slice->root_eviction_priority, &slice->stats);
    bool already_existed = kv_location.value.has();

    scoped_malloc_t<rdb_value_t> new_value(MAX_RDB_VALUE_SIZE);
    bzero(new_value.get(), MAX_RDB_VALUE_SIZE);

    //TODO unnecessary copies they must go away.
    write_message_t wm;
    wm << data;
    vector_stream_t stream;
    int res = send_write_message(&stream, &wm);
    guarantee_err(res == 0, "Serialization for json data failed... this shouldn't happen.\n");

    blob_t blob(new_value->value_ref(), blob::btree_maxreflen);

    //TODO more copies, good lord
    blob.append_region(txn, stream.vector().size());
    std::string sered_data(stream.vector().begin(), stream.vector().end());
    blob.write_from_string(sered_data, txn, 0);

    // Actually update the leaf, if needed.
    kv_location.value.reinterpret_swap(new_value);
    null_key_modification_callback_t<rdb_value_t> null_cb;
    apply_keyvalue_change(txn, &kv_location, key.btree_key(), timestamp, false, &null_cb, &slice->root_eviction_priority);
    //                                                                     ^-- That means the key isn't expired.

    return point_write_response_t(already_existed ? DUPLICATE : STORED);
}

class agnostic_rdb_backfill_callback_t : public agnostic_backfill_callback_t {
public:
    agnostic_rdb_backfill_callback_t(rdb_backfill_callback_t *cb, const key_range_t &kr) : cb_(cb), kr_(kr) { }

    void on_delete_range(const key_range_t &range) {
        rassert(kr_.is_superset(range));
        cb_->on_delete_range(range);
    }

    void on_deletion(const btree_key_t *key, repli_timestamp_t recency) {
        rassert(kr_.contains_key(key->contents, key->size));
        cb_->on_deletion(key, recency);
    }

    void on_pair(transaction_t *txn, repli_timestamp_t recency, const btree_key_t *key, const void *val) {
        rassert(kr_.contains_key(key->contents, key->size));
        const rdb_value_t *value = static_cast<const rdb_value_t *>(val);

        rdb_protocol_details::backfill_atom_t atom;
        atom.key.assign(key->size, key->contents);
        atom.value = get_data(value, txn);
        atom.recency = recency;
        cb_->on_keyvalue(atom);
    }

    rdb_backfill_callback_t *cb_;
    key_range_t kr_;
};

void rdb_backfill(btree_slice_t *slice, const key_range_t& key_range, repli_timestamp_t since_when, rdb_backfill_callback_t *callback,
                    transaction_t *txn, superblock_t *superblock, parallel_traversal_progress_t *p) {
    agnostic_rdb_backfill_callback_t agnostic_cb(callback, key_range);
    value_sizer_t<rdb_value_t> sizer(slice->cache()->get_block_size());
    do_agnostic_btree_backfill(&sizer, slice, key_range, since_when, &agnostic_cb, txn, superblock, p);
}

point_delete_response_t rdb_delete(const store_key_t &key, btree_slice_t *slice, repli_timestamp_t timestamp, transaction_t *txn, superblock_t *superblock) {
    keyvalue_location_t<rdb_value_t> kv_location;
    find_keyvalue_location_for_write(txn, superblock, key.btree_key(), &kv_location, &slice->root_eviction_priority, &slice->stats);
    bool exists = kv_location.value.has();
    if(exists) {
        blob_t blob(kv_location.value->value_ref(), blob::btree_maxreflen);
        blob.clear(txn);
        kv_location.value.reset();
        null_key_modification_callback_t<rdb_value_t> null_cb;
        apply_keyvalue_change(txn, &kv_location, key.btree_key(), timestamp, false, &null_cb, &slice->root_eviction_priority);
    }

    return point_delete_response_t(exists ? DELETED : MISSING);
}

void rdb_erase_range(btree_slice_t *slice, key_tester_t *tester,
                       bool left_key_supplied, const store_key_t& left_key_exclusive,
                       bool right_key_supplied, const store_key_t& right_key_inclusive,
                       transaction_t *txn, superblock_t *superblock) {

    value_sizer_t<rdb_value_t> rdb_sizer(slice->cache()->get_block_size());
    value_sizer_t<void> *sizer = &rdb_sizer;

    struct : public value_deleter_t {
        void delete_value(transaction_t *txn, void *value) {
            blob_t blob(static_cast<rdb_value_t *>(value)->value_ref(), blob::btree_maxreflen);
            blob.clear(txn);
        }
    } deleter;

    btree_erase_range_generic(sizer, slice, tester, &deleter,
        left_key_supplied ? left_key_exclusive.btree_key() : NULL,
        right_key_supplied ? right_key_inclusive.btree_key() : NULL,
        txn, superblock);
}

void rdb_erase_range(btree_slice_t *slice, key_tester_t *tester,
                       const key_range_t &keys,
                       transaction_t *txn, superblock_t *superblock) {
    store_key_t left_exclusive(keys.left);
    store_key_t right_inclusive(keys.right.key);

    bool left_key_supplied = left_exclusive.decrement();
    bool right_key_supplied = !keys.right.unbounded;
    if (right_key_supplied) {
        right_inclusive.decrement();
    }
    rdb_erase_range(slice, tester, left_key_supplied, left_exclusive, right_key_supplied, right_inclusive, txn, superblock);
}

size_t estimate_rget_response_size(const boost::shared_ptr<scoped_cJSON_t> &/*json*/) {
    // TODO: don't be stupid, be a smarty, come and join the nazy
    // party (json size estimation will be much easier once we switch
    // to bson -- fuck it for now).
    return 250;
}

typedef std::list<boost::shared_ptr<scoped_cJSON_t> > json_list_t;

/* A visitor for applying a transformation to a bit of json. */
class transform_visitor_t : public boost::static_visitor<void> {
public:
    transform_visitor_t(boost::shared_ptr<scoped_cJSON_t> _json, json_list_t *_out, query_language::runtime_environment_t *_env)
        : json(_json), out(_out), env(_env)
    { }

    void operator()(const Builtin_Filter &filter) const {
        query_language::backtrace_t b; //TODO get this from somewhere
        if (query_language::predicate_t(filter.predicate(), *env, b)(json)) {
            out->push_back(json);
        }
    }

    void operator()(const Builtin_Map &map) const {
        query_language::backtrace_t b; //TODO get this from somewhere
        Term t = map.mapping().body();
        out->push_back(query_language::map(map.mapping().arg(), &t, *env, json, b));
    }

    void operator()(const Builtin_ConcatMap &concatmap) const {
        query_language::backtrace_t b; //TODO get this from somewhere
        Term t = concatmap.mapping().body();
        boost::shared_ptr<json_stream_t> stream = query_language::concatmap(concatmap.mapping().arg(), &t, *env, json, b);
        while (boost::shared_ptr<scoped_cJSON_t> data = stream->next()) {
            out->push_back(data);
        }
    }

    void operator()(Builtin_Range range) const {
        boost::shared_ptr<scoped_cJSON_t> lowerbound, upperbound;
        query_language::backtrace_t b; //TODO get this from somewhere

        key_range_t key_range;

        /* TODO this is inefficient because it involves recomputing this for each element. */
        if (range.has_lowerbound()) {
            lowerbound = eval(range.mutable_lowerbound(), env, b.with("lowerbound"));
        }

        if (range.has_upperbound()) {
            upperbound = eval(range.mutable_upperbound(), env, b.with("upperbound"));
        }

        if (lowerbound && upperbound) {
            key_range = key_range_t(key_range_t::closed, store_key_t(lowerbound->Print()),
                    key_range_t::closed, store_key_t(upperbound->Print()));
        } else if (lowerbound) {
            key_range = key_range_t(key_range_t::closed, store_key_t(lowerbound->Print()),
                    key_range_t::none, store_key_t());
        } else if (upperbound) {
            key_range = key_range_t(key_range_t::none, store_key_t(),
                    key_range_t::closed, store_key_t(upperbound->Print()));
        }


        cJSON* val = json->GetObjectItem(range.attrname().c_str());

        if (val && key_range.contains_key(store_key_t(cJSON_print_std_string(val)))) {
            out->push_back(json);
        }
    }

private:
    boost::shared_ptr<scoped_cJSON_t> json;
    json_list_t *out;
    query_language::runtime_environment_t *env;
};

/* A visitor for setting the result type based on a terminal. */
class terminal_initializer_visitor_t : public boost::static_visitor<void> {
public:
    terminal_initializer_visitor_t(rget_read_response_t::result_t *_out)
        : out(_out)
    { }

    void operator()(const Builtin_GroupedMapReduce &) const { *out = rget_read_response_t::groups_t(); }
        
    void operator()(const Reduction &) const { *out = rget_read_response_t::atom_t(); }

    void operator()(const rdb_protocol_details::Length &) const { *out = rget_read_response_t::length_t(); }

    void operator()(const WriteQuery_ForEach &) const { *out = rget_read_response_t::inserted_t(); }
private:
    rget_read_response_t::result_t *out;
};

/* A visitor for applying a terminal to a bit of json. */
class terminal_visitor_t : public boost::static_visitor<void> {
public:
    terminal_visitor_t(boost::shared_ptr<scoped_cJSON_t> _json, 
                       query_language::runtime_environment_t *_env, 
                       rget_read_response_t::result_t *_out)
        : json(_json), env(_env), out(_out)
    { }

    void operator()(const Builtin_GroupedMapReduce &gmr) const { 
        boost::shared_ptr<scoped_cJSON_t> json_cpy = json;
        query_language::backtrace_t b; //TODO get this from somewhere
        //we assume the result has already been set to groups_t
        rget_read_response_t::groups_t *res_groups = boost::get<rget_read_response_t::groups_t>(out);
        guarantee(res_groups);

        //Grab the grouping
        boost::shared_ptr<scoped_cJSON_t> grouping;
        {
            query_language::new_val_scope_t scope(&env->scope);
            Term body = gmr.group_mapping().body();

            env->scope.put_in_scope(gmr.group_mapping().arg(), json_cpy);
            grouping = eval(&body, env, b);
        }

        //Apply the mapping
        {
            query_language::new_val_scope_t scope(&env->scope);
            env->scope.put_in_scope(gmr.value_mapping().arg(), json_cpy);

            Term body = gmr.value_mapping().body();
            json_cpy = eval(&body, env, b);
        }

        //Finally reduce it in
        {
            query_language::new_val_scope_t scope(&env->scope);

            Term base = gmr.reduction().base(),
                 body = gmr.reduction().body();

            env->scope.put_in_scope(gmr.reduction().var1(), get_with_default(*res_groups, grouping, eval(&base, env, b)));
            env->scope.put_in_scope(gmr.reduction().var2(), json_cpy);
            (*res_groups)[grouping] = eval(&body, env, b);
        }
    }
        
    void operator()(const Reduction &r) const { 
        query_language::backtrace_t b; //TODO get this from somewhere
        //we assume the result has already been set to groups_t
        rget_read_response_t::atom_t *res_atom = boost::get<rget_read_response_t::atom_t>(out);
        guarantee(res_atom);

        query_language::new_val_scope_t scope(&env->scope);
        env->scope.put_in_scope(r.var1(), *res_atom);
        env->scope.put_in_scope(r.var2(), json);
        Term body = r.body();
        *res_atom = eval(&body, env, b);
    }

    void operator()(const rdb_protocol_details::Length &) const {
        rget_read_response_t::length_t *res_length = boost::get<rget_read_response_t::length_t>(out);
        guarantee(res_length);
        res_length->length++;
    }

    void operator()(const WriteQuery_ForEach &w) const {
        query_language::backtrace_t b; //TODO get this from somewhere

        query_language::new_val_scope_t scope(&env->scope);
        env->scope.put_in_scope(w.var(), json);

        for (int i = 0; i < w.queries_size(); ++i) {
            WriteQuery q = w.queries(i);
            Response r; //TODO we need to actually return this somewhere I suppose.
            execute(&q, env, &r, b);
        }
    }

private:
    boost::shared_ptr<scoped_cJSON_t> json;
    query_language::runtime_environment_t *env;
    rget_read_response_t::result_t *out;
};


class rdb_rget_depth_first_traversal_callback_t : public depth_first_traversal_callback_t {
public:
    rdb_rget_depth_first_traversal_callback_t(transaction_t *txn, int max,
                                              query_language::runtime_environment_t *_env,
                                              const rdb_protocol_details::transform_t &_transform,
                                              boost::optional<rdb_protocol_details::terminal_t> _terminal) 
        : transaction(txn), maximum(max), cumulative_size(0),
          env(_env), transform(_transform), terminal(_terminal)
    { }
    bool handle_pair(const btree_key_t* key, const void *value) {
        store_key_t store_key(key);
        if (response.last_considered_key < store_key) {
            response.last_considered_key = store_key;
        }

        const rdb_value_t *rdb_value = reinterpret_cast<const rdb_value_t *>(value);

        json_list_t data;
        data.push_back(get_data(rdb_value, transaction));

        //Apply transforms to the data
        typedef rdb_protocol_details::transform_t::iterator tit_t;
        for (tit_t it  = transform.begin();
                   it != transform.end();
                   ++it) {
             json_list_t tmp;

            for (json_list_t::iterator jt  = data.begin();
                                       jt != data.end();
                                       ++jt) {
                boost::apply_visitor(transform_visitor_t(*jt, &tmp, env), *it);
            }
            data.clear();
            data.splice(data.begin(), tmp);
        }

        if (!terminal) {
            typedef rget_read_response_t::stream_t stream_t;
            stream_t *stream = boost::get<stream_t>(&response.result);
            guarantee(stream);
            stream->insert(stream->end(), data.begin(), data.end()); //why is this a vector? if it was a list we could just splice and things would be nice.

            cumulative_size += estimate_rget_response_size(stream->back());
            return int(stream->size()) < maximum && cumulative_size < rget_max_chunk_size;
        } else {
            boost::apply_visitor(terminal_initializer_visitor_t(&response.result), *terminal);
            for (json_list_t::iterator jt  = data.begin();
                                       jt != data.end();
                                       ++jt) {
                boost::apply_visitor(terminal_visitor_t(*jt, env, &response.result), *terminal);
            }
            return true;
        }
    }
    transaction_t *transaction;
    int maximum;
    rget_read_response_t response;
    size_t cumulative_size;
    query_language::runtime_environment_t *env;
    rdb_protocol_details::transform_t transform;
    boost::optional<rdb_protocol_details::terminal_t> terminal;
};

rget_read_response_t rdb_rget_slice(btree_slice_t *slice, const key_range_t &range,
                                    int maximum, transaction_t *txn, superblock_t *superblock,
                                    query_language::runtime_environment_t *env, const rdb_protocol_details::transform_t &transform,
                                    boost::optional<rdb_protocol_details::terminal_t> terminal) {

    rdb_rget_depth_first_traversal_callback_t callback(txn, maximum, env, transform, terminal);
    btree_depth_first_traversal(slice, txn, superblock, range, &callback);
    if (callback.cumulative_size >= rget_max_chunk_size) {
        callback.response.truncated = true;
    } else {
        callback.response.truncated = false;
    }
    return callback.response;
}
