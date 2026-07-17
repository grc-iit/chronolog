/*
 * kafka_read_bench.cpp -- Kafka READ benchmark using librdkafka.
 *
 * Consumes one partition with manual offset control:
 *   - archive: seek to BEGINNING, consume to the high watermark (full scan)
 *   - tail:    seek to (high_watermark - tail_k), consume tail_k (recent slice)
 * Assumes the partition is already loaded. Prints one stdout line:
 *   <readtype> <events_read> <payload_bytes> <t_start_epoch> <t_end_epoch>
 *
 * Usage: kafka_read_bench <brokers> <topic> <partition> <mode:archive|tail> <tail_k>
 */
#include <librdkafka/rdkafka.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include "bench_common.h"

int main(int argc, char** argv)
{
    if(argc < 6)
    {
        fprintf(stderr, "args: brokers topic partition mode(archive|tail) tail_k\n");
        return 2;
    }
    const char *brokers = argv[1], *topic = argv[2];
    int32_t part = atoi(argv[3]);
    std::string mode = argv[4];
    int64_t tail_k = strtoll(argv[5], 0, 10);

    char errstr[512];
    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    rd_kafka_conf_set(conf, "bootstrap.servers", brokers, errstr, sizeof errstr);
    std::string gid = "rb" + std::to_string(getpid()) + "_" + std::to_string(part);
    rd_kafka_conf_set(conf, "group.id", gid.c_str(), errstr, sizeof errstr);
    rd_kafka_conf_set(conf, "enable.auto.commit", "false", errstr, sizeof errstr);
    rd_kafka_conf_set(conf, "fetch.min.bytes", "1", errstr, sizeof errstr);
    rd_kafka_conf_set(conf, "queued.max.messages.kbytes", "1048576", errstr, sizeof errstr);

    rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof errstr);
    if(!rk)
    {
        fprintf(stderr, "consumer new: %s\n", errstr);
        return 1;
    }

    // Watermarks for this partition.
    int64_t lo = 0, hi = 0;
    rd_kafka_query_watermark_offsets(rk, topic, part, &lo, &hi, 5000);
    int64_t start, target;
    if(mode == "tail")
    {
        start = (hi - tail_k > lo) ? (hi - tail_k) : lo;
        target = hi - start;
    }
    else
    { // archive
        start = lo;
        target = hi - lo;
    }

    rd_kafka_topic_partition_list_t* parts = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(parts, topic, part)->offset = start;
    rd_kafka_assign(rk, parts);

    uint64_t count = 0, bytes = 0;
    double t0 = now_sec(), t1 = t0;
    int idle = 0;
    while((int64_t)count < target)
    {
        rd_kafka_message_t* m = rd_kafka_consumer_poll(rk, 1000);
        if(!m)
        {
            if(++idle > 5)
                break;
            continue;
        }
        if(!m->err)
        {
            count++;
            bytes += m->len;
            idle = 0;
        }
        rd_kafka_message_destroy(m);
    }
    t1 = now_sec();

    size_t plen = bytes && count ? bytes / count : 0;
    printf("%s %llu %zu %.6f %.6f\n", mode.c_str(), (unsigned long long)count, plen, t0, t1);

    rd_kafka_topic_partition_list_destroy(parts);
    rd_kafka_consumer_close(rk);
    rd_kafka_destroy(rk);
    return 0;
}
