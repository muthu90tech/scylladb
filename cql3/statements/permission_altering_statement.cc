/*
 * Copyright 2016-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: (AGPL-3.0-or-later and Apache-2.0)
 */

#include <seastar/core/thread.hh>

#include "auth/service.hh"
#include "permission_altering_statement.hh"
#include "cql3/functions/functions.hh"
#include "cql3/functions/user_aggregate.hh"
#include "cql3/functions/user_function.hh"
#include "cql3/query_processor.hh"
#include "cql3/query_options.hh"
#include "cql3/role_name.hh"
#include "cql3/selection/selection.hh"
#include "gms/feature_service.hh"

static auth::permission_set filter_applicable_permissions(const auth::permission_set& ps, const auth::resource& r) {
    auto const filtered_permissions = auth::permission_set::from_mask(ps.mask() & r.applicable_permissions().mask());

    if (!filtered_permissions) {
        throw exceptions::syntax_exception(
                format("Resource {} does not support any of the requested permissions.", r));
    }

    return filtered_permissions;
}

cql3::statements::permission_altering_statement::permission_altering_statement(
                auth::permission_set permissions, auth::resource resource,
                const role_name& rn)
                : _permissions(filter_applicable_permissions(permissions, resource))
                , _resource(std::move(resource))
                , _role_name(rn.to_string()) {
}

void cql3::statements::permission_altering_statement::validate(query_processor&, const service::client_state&) const {
}

future<> cql3::statements::permission_altering_statement::check_access(query_processor& qp, const service::client_state& state) const {
    state.ensure_not_anonymous();
    maybe_correct_resource(_resource, state, qp);

    return state.ensure_exists(_resource).then([this, &state] {
        if (_resource.kind() == auth::resource_kind::functions) {
            // Even if the function exists, it may be a builtin function, in which case we disallow altering permissions on it.
            auth::functions_resource_view v(_resource);
            if (v.function_signature()) {
                // If the resource has a signature, it is a specific funciton and not "all functions"
                auto [name, function_args] = auth::decode_signature(*v.function_signature());
                auto fun = cql3::functions::functions::find(db::functions::function_name{sstring(*v.keyspace()), name}, function_args);
                if (fun->is_native()) {
                    return make_exception_future<>(exceptions::invalid_request_exception("Altering permissions on builtin functions is not supported"));
                }
            }
        }
        // check that the user has AUTHORIZE permission on the resource or its parents, otherwise reject
        // GRANT/REVOKE.
        return state.ensure_has_permission({auth::permission::AUTHORIZE, _resource}).then([this, &state] {
            return do_for_each(_permissions, [this, &state](auth::permission p) {
                // TODO: how about we re-write the access check to check a set
                // right away.
                return state.ensure_has_permission({p, _resource});
            });
        });
    });
}
