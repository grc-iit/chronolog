//
// Created by kfeng on 11/4/21.
//

#include <cassert>
#include <iostream>
#include <string>
#include <chrono>
#include <rocksdb/db.h>
#include <mpi.h>

#define NUM_KEYS (1024 * 1024)
#define READ_ONLY 1

using namespace std;

int main(int argc, char**argv)
{
    MPI_Init(&argc, &argv);

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    rocksdb::DB*db;
    rocksdb::Options options;
    options.create_if_missing = true;
#ifndef READ_ONLY
    rocksdb::Status status =
            rocksdb::DB::Open(options, "/tmp/testdb", &db);
#else
    rocksdb::Status status = rocksdb::DB::OpenForReadOnly(options, "/tmp/testdb", &db);
#endif
    assert(status.ok());

#ifndef READ_ONLY
    // Insert values
    for (int i = 0; i < NUM_KEYS; i++) {
        status = db->Put(rocksdb::WriteOptions(),
                         "Test key" + to_string(i),
                         "Test value" + to_string(i + 1));
        assert(status.ok());
    }
#endif
    auto start = chrono::steady_clock::now();

    // Read back value
    std::string value;
    for(int i = 0; i < NUM_KEYS; i++)
    {
        status = db->Get(rocksdb::ReadOptions(), "Test key" + to_string(i), &value);
        assert(status.ok());
        assert(!status.IsNotFound());
    }

    auto end = chrono::steady_clock::now();
    cout << "Elapsed time in milliseconds: " << chrono::duration_cast <chrono::milliseconds>(end - start).count()
         << " ms" << endl;

    // Read key which does not exist
    status = db->Get(rocksdb::ReadOptions(), "This key does not exist", &value);
    assert(status.IsNotFound());

    MPI_Finalize();
}
