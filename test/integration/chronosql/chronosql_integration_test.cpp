#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "chronosql.h"

// ChronoSQL integration test (MANUAL).
//
// Exercises the full plugin against a running ChronoLog deployment using a
// small YCSB-shaped workload: CREATE TABLE, INSERT (programmatic + SQL),
// SELECT * (full scan), SELECT WHERE col=, SELECT WHERE __ts BETWEEN.
//
// Does not auto-run in CI: the test is registered MANUAL TRUE in CMake.

namespace
{

bool expect_eq(const char* what, std::size_t actual, std::size_t expected)
{
    if(actual != expected)
    {
        std::cerr << "[FAIL] " << what << ": expected " << expected << " got " << actual << "\n";
        return false;
    }
    std::cout << "[OK]   " << what << ": " << actual << "\n";
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::string conf_path;
    for(int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if(a == "-c" || a == "--config")
        {
            if(i + 1 < argc)
            {
                conf_path = argv[++i];
            }
        }
    }

    chronosql::ChronoSQL db = conf_path.empty() ? chronosql::ChronoSQL() : chronosql::ChronoSQL(conf_path);

    const std::string table = "ycsb_users";
    if(!db.getSchema(table).has_value())
    {
        db.execute("CREATE TABLE " + table + " (id INT, name STRING, region STRING)");
    }

    constexpr int N = 100;
    std::vector<std::uint64_t> ts;
    ts.reserve(N);

    auto t0 = std::chrono::steady_clock::now();
    for(int i = 0; i < N; ++i)
    {
        std::string region = (i % 2 == 0) ? "us-east" : "us-west";
        std::string sql = "INSERT INTO " + table + " (id, name, region) VALUES (" + std::to_string(i) + ", 'user" +
                          std::to_string(i) + "', '" + region + "')";
        auto r = db.execute(sql);
        if(!r.last_insert_timestamp.has_value())
        {
            std::cerr << "INSERT did not return a timestamp\n";
            return EXIT_FAILURE;
        }
        ts.push_back(*r.last_insert_timestamp);
    }
    db.flush();
    std::this_thread::sleep_for(std::chrono::seconds(2));

    bool ok = true;
    auto all = db.execute("SELECT * FROM " + table);
    ok &= expect_eq("SELECT * row count", all.rows.size(), N);

    auto east = db.execute("SELECT id, region FROM " + table + " WHERE region = 'us-east'");
    ok &= expect_eq("SELECT WHERE region='us-east'", east.rows.size(), N / 2);

    auto by_id = db.execute("SELECT * FROM " + table + " WHERE id = 42");
    ok &= expect_eq("SELECT WHERE id=42", by_id.rows.size(), 1u);

    if(N >= 10)
    {
        auto range = db.execute("SELECT * FROM " + table + " WHERE __ts BETWEEN " + std::to_string(ts[10]) + " AND " +
                                std::to_string(ts[19]));
        ok &= expect_eq("SELECT WHERE __ts BETWEEN ts[10] AND ts[19] (inclusive)", range.rows.size(), 10u);
    }

    auto t1 = std::chrono::steady_clock::now();
    std::cout << "Total time: " << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << " ms\n";

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
