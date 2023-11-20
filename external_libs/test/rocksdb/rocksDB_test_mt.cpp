//
// Created by kfeng on 11/4/21.
//

#include <cassert>
#include <string>
#include <chrono>
#include <thread>
#include <rocksdb/db.h>
#include <iostream>

#define NUM_KEYS (1024 * 1024)
#define NUM_READER_THRDS 8
#define READ_ONLY 1

using namespace std;

void writer_thread_func()
{
    rocksdb::DB*db;
    rocksdb::Options options;
    options.create_if_missing = true;

    rocksdb::Status status = rocksdb::DB::Open(options, "/tmp/testdb", &db);
    assert(status.ok());

    // Insert values
    for(int i = 0; i < NUM_KEYS; i++)
    {
        status = db->Put(rocksdb::WriteOptions(), "Test key" + to_string(i), "Test value" + to_string(i + 1));
        assert(status.ok());
    }
}

void reader_thread_func()
{
    std::this_thread::sleep_for(std::chrono::seconds(5));

    rocksdb::DB*db;
    rocksdb::Options options;
    options.create_if_missing = true;

    rocksdb::Status status = rocksdb::DB::OpenForReadOnly(options, "/tmp/testdb", &db);
    assert(status.ok());

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
}

int main(int argc, char**argv)
{
    thread writer_thr, reader_thr[NUM_READER_THRDS];

    writer_thr = thread(writer_thread_func);

    for(int i = 0; i < NUM_READER_THRDS; i++)
    {
        reader_thr[i] = thread(reader_thread_func);
    }

    writer_thr.join();

    for(int i = 0; i < NUM_READER_THRDS; i++)
    {
        reader_thr[i].join();
    }
}
