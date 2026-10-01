#pragma once
#include "catalog/SqliteMetadataStore.h"
namespace chronolog::visor::dynamic
{
internal::v1::MembershipState snapshot(SqliteMetadataStore& store);
std::string apply(SqliteMetadataStore& store, const internal::v1::MembershipCommand& command);
RouteState routeState(const internal::v1::RouteUpdate& update);
} // namespace chronolog::visor::dynamic
