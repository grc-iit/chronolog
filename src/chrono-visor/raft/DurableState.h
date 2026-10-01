#pragma once
#include <libnuraft/nuraft.hxx>
#include <mutex>
#include <sqlite3.h>
#include "VisorConfig.h"
namespace chronolog::visor
{
class DurableState final
    : public nuraft::log_store
    , public nuraft::state_mgr
{
public:
    DurableState(const std::string& path, RaftConfig config);
    ~DurableState() override;
    nuraft::ulong next_slot() const override;
    nuraft::ulong start_index() const override;
    nuraft::ptr<nuraft::log_entry> last_entry() const override;
    nuraft::ulong append(nuraft::ptr<nuraft::log_entry>& entry) override;
    void write_at(nuraft::ulong index, nuraft::ptr<nuraft::log_entry>& entry) override;
    nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>> log_entries(nuraft::ulong start,
                                                                         nuraft::ulong end) override;
    nuraft::ptr<nuraft::log_entry> entry_at(nuraft::ulong index) override;
    nuraft::ulong term_at(nuraft::ulong index) override;
    nuraft::ptr<nuraft::buffer> pack(nuraft::ulong index, int32_t count) override;
    void apply_pack(nuraft::ulong index, nuraft::buffer& pack) override;
    bool compact(nuraft::ulong index) override;
    bool flush() override;
    nuraft::ptr<nuraft::cluster_config> load_config() override;
    void save_config(const nuraft::cluster_config& config) override;
    void save_state(const nuraft::srv_state& state) override;
    nuraft::ptr<nuraft::srv_state> read_state() override;
    nuraft::ptr<nuraft::log_store> load_log_store() override;
    int32_t server_id() override;
    void system_exit(int code) override;
    nuraft::ptr<nuraft::buffer> get(const std::string& key) const;
    void put(const std::string& key, const nuraft::buffer& value);

private:
    void sql(const std::string& query) const;
    uint64_t scalar(const std::string& query) const;
    void insert(uint64_t index, const nuraft::buffer& value);
    sqlite3* db_{};
    RaftConfig config_;
    mutable std::recursive_mutex mutex_;
};
} // namespace chronolog::visor
