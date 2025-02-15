/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#pragma once

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/base/data_view.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings_gen.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/trusted_hasher.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {
/**
 * Truncates the 256 bit QueryShapeHash by taking only the first sizeof(size_t) bytes.
 */
class QueryShapeHashHasher {
public:
    size_t operator()(const query_shape::QueryShapeHash& hash) const {
        return ConstDataView(reinterpret_cast<const char*>(hash.data())).read<size_t>();
    }
};
template <>
struct IsTrustedHasher<QueryShapeHashHasher, query_shape::QueryShapeHash> : std::true_type {};

namespace query_settings {

using QueryInstance = BSONObj;

using QueryShapeConfigurationsMap = stdx::unordered_map<query_shape::QueryShapeHash,
                                                        std::pair<QuerySettings, QueryInstance>,
                                                        QueryShapeHashHasher>;

/**
 * Struct stores all query shape configurations for a given tenant. It stores the same information
 * as QuerySettingsClusterParameterValue. The data present in the 'settingsArray' is stored in the
 * QueryShapeConfigurationsMap for faster access.
 */
struct VersionedQueryShapeConfigurations {
    /**
     * An unordered_map of all QueryShapeConfigurations stored within the
     * QuerySettingsClusterParameter keyed by QueryShapHash.
     */
    QueryShapeConfigurationsMap queryShapeConfigurationsMap;

    /**
     * Cluster time of the current version of the QuerySettingsClusterParameter.
     */
    LogicalTime clusterParameterTime;
};

/**
 * Class responsible for managing in-memory storage and fetching of query settings. The in-memory
 * storage is eventually consistent with the query settings on other cluster nodes and is updated
 * based on OpObserver call performed when executing setClusterParameter command.
 *
 * Query settings in-memory storage is maintained separately for each tenant. In dedicated
 * environments the 'tenantId' argument passed to the methods must be boost::none.
 *
 * Query settings should only be retrieved through this class.
 */
class QuerySettingsManager {
public:
    static constexpr auto kQuerySettingsClusterParameterName = "querySettings"_sd;
    explicit QuerySettingsManager(ServiceContext* service) {}

    ~QuerySettingsManager() = default;

    QuerySettingsManager(const QuerySettingsManager&) = delete;
    QuerySettingsManager& operator=(const QuerySettingsManager&) = delete;

    static void create(ServiceContext* service);

    static QuerySettingsManager& get(ServiceContext* service);
    static QuerySettingsManager& get(OperationContext* opCtx);

    /**
     * Performs the QuerySettings lookup by computing QueryShapeHash only in cases when at least one
     * QueryShapeConfiguration is set.
     */
    boost::optional<std::pair<QuerySettings, QueryInstance>> getQuerySettingsForQueryShapeHash(
        OperationContext* opCtx,
        std::function<query_shape::QueryShapeHash(void)> queryShapeHashFn,
        const boost::optional<TenantId>& tenantId) const;

    /**
     * Returns (QuerySettings, QueryInstance) pair associated with the QueryShapeHash for the given
     * tenant.
     */
    boost::optional<std::pair<QuerySettings, QueryInstance>> getQuerySettingsForQueryShapeHash(
        OperationContext* opCtx,
        const query_shape::QueryShapeHash& queryShapeHash,
        const boost::optional<TenantId>& tenantId) const {
        return getQuerySettingsForQueryShapeHash(
            opCtx, [&]() { return queryShapeHash; }, tenantId);
    }

    /**
     * Returns all QueryShapeConfigurations stored for the given tenant.
     */
    std::vector<QueryShapeConfiguration> getAllQueryShapeConfigurations(
        OperationContext* opCtx, const boost::optional<TenantId>& tenantId) const;

    /**
     * Sets the QueryShapeConfigurations by replacing an existing VersionedQueryShapeConfigurations
     * with the newly built one.
     */
    void setQueryShapeConfigurations(OperationContext* opCtx,
                                     std::vector<QueryShapeConfiguration>&& settings,
                                     LogicalTime parameterClusterTime,
                                     const boost::optional<TenantId>& tenantId);

    /**
     * Removes all query settings documents for the given tenant.
     */
    void removeAllQueryShapeConfigurations(OperationContext* opCtx,
                                           const boost::optional<TenantId>& tenantId);

    /**
     * Returns the cluster parameter time of the current QuerySettingsClusterParameter value for the
     * given tenant.
     */
    LogicalTime getClusterParameterTime(OperationContext* opCtx,
                                        const boost::optional<TenantId>& tenantId) const;

    /**
     * Appends the QuerySettingsClusterParameterValue maintained as
     * VersionedQueryShapeConfigurations for the given tenant.
     */
    void appendQuerySettingsClusterParameterValue(OperationContext* opCtx,
                                                  BSONObjBuilder* bob,
                                                  const boost::optional<TenantId>& tenantId);

private:
    std::vector<QueryShapeConfiguration> getAllQueryShapeConfigurations_inlock(
        OperationContext* opCtx, const boost::optional<TenantId>& tenantId) const;

    LogicalTime getClusterParameterTime_inlock(OperationContext* opCtx,
                                               const boost::optional<TenantId>& tenantId) const;

    TenantIdMap<VersionedQueryShapeConfigurations> _tenantIdToVersionedQueryShapeConfigurationsMap;
    Lock::ResourceMutex _mutex = Lock::ResourceMutex("QuerySettingsManager::mutex");
};
};  // namespace query_settings
}  // namespace mongo
