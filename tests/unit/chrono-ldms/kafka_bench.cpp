/*
 * kafka_bench.cpp -- Kafka producer benchmark using librdkafka (the same
 * client library the LDMS store_kafka plugin uses).
 *
 * T producer threads each send total_events/T records (one per Kafka
 * partition) to the topic. We report two throughputs:
 *   - enqueue: rate the client accepts records into its send queue (async).
 *   - acked:   rate including rd_kafka_flush(), i.e. broker-acknowledged.
 * The "acked" number is the fair cross-backend metric (server confirmed the
 * data), comparable to ChronoLog's synchronous per-event keeper ack.
 *
 * Usage: kafka_bench <brokers> <topic> <payload_size> <total_events> <threads> <acks>
 */
#include <librdkafka/rdkafka.h>

#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#include "bench_common.h"

static std::atomic<uint64_t> g_delivered{0};
static std::atomic<uint64_t> g_errors{0};

static void dr_cb(rd_kafka_t *rk, const rd_kafka_message_t *msg, void *opaque)
{
	(void)rk; (void)opaque;
	if (msg->err)
		g_errors.fetch_add(1, std::memory_order_relaxed);
	else
		g_delivered.fetch_add(1, std::memory_order_relaxed);
}

int main(int argc, char **argv)
{
	if (argc < 7) {
		fprintf(stderr, "usage: %s <brokers> <topic> <payload_size> "
			"<total_events> <threads> <acks>\n", argv[0]);
		return 2;
	}
	const char *brokers = argv[1];
	const char *topic   = argv[2];
	size_t   payload_sz = strtoul(argv[3], nullptr, 10);
	uint64_t total      = strtoull(argv[4], nullptr, 10);
	int      threads    = atoi(argv[5]);
	const char *acks    = argv[6];
	int      base_part  = (argc > 7) ? atoi(argv[7]) : 0; // partition offset

	std::string payload = make_payload(payload_sz);
	size_t plen = payload.size();

	// KAFKA_SYNC=1 -> cripple Kafka down to ChronoLog's mode: one record per
	// request, no pipelining, blocking on a single-node ack per record.
	bool sync = (getenv("KAFKA_SYNC") != nullptr);

	char errstr[512];
	rd_kafka_conf_t *conf = rd_kafka_conf_new();
	rd_kafka_conf_set(conf, "bootstrap.servers", brokers, errstr, sizeof errstr);
	rd_kafka_conf_set(conf, "acks", acks, errstr, sizeof errstr);
	if (sync) {
		rd_kafka_conf_set(conf, "batch.num.messages", "1", errstr, sizeof errstr);
		rd_kafka_conf_set(conf, "linger.ms", "0", errstr, sizeof errstr);
		rd_kafka_conf_set(conf, "max.in.flight.requests.per.connection", "1", errstr, sizeof errstr);
		rd_kafka_conf_set(conf, "enable.idempotence", "false", errstr, sizeof errstr);
	} else {
		// Large client-side queue so enqueue is not throttled prematurely.
		rd_kafka_conf_set(conf, "queue.buffering.max.messages", "4000000", errstr, sizeof errstr);
		rd_kafka_conf_set(conf, "queue.buffering.max.kbytes", "2097151", errstr, sizeof errstr);
		rd_kafka_conf_set(conf, "linger.ms", "5", errstr, sizeof errstr);
	}
	rd_kafka_conf_set_dr_msg_cb(conf, dr_cb);

	rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof errstr);
	if (!rk) { fprintf(stderr, "rd_kafka_new: %s\n", errstr); return 1; }

	rd_kafka_topic_t *rkt = rd_kafka_topic_new(rk, topic, nullptr);
	if (!rkt) { fprintf(stderr, "topic_new failed\n"); return 1; }

	uint64_t per = total / threads;
	std::atomic<uint64_t> produced{0};

	double t0 = now_sec();
	std::vector<std::thread> pool;
	for (int t = 0; t < threads; t++) {
		pool.emplace_back([&, t]() {
			int32_t part = base_part + t; // one partition per writer
			for (uint64_t i = 0; i < per; i++) {
			retry:
				int rc = rd_kafka_produce(
					rkt, part, RD_KAFKA_MSG_F_COPY,
					(void *)payload.data(), plen,
					nullptr, 0, nullptr);
				if (rc == -1) {
					if (rd_kafka_last_error() ==
					    RD_KAFKA_RESP_ERR__QUEUE_FULL) {
						rd_kafka_poll(rk, 10);
						goto retry;
					}
					g_errors.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				if (sync)
					// Block until THIS record is broker-acked
					// before producing the next: one event, one
					// round-trip (ChronoLog-equivalent mode).
					rd_kafka_flush(rk, 10000);
				else if ((i & 0x3FF) == 0)
					rd_kafka_poll(rk, 0);
			}
			produced.fetch_add(per, std::memory_order_relaxed);
		});
	}
	for (auto &th : pool) th.join();
	double t1 = now_sec();           // all records enqueued

	rd_kafka_flush(rk, 120000);      // wait for broker acks
	double t2 = now_sec();           // all records acknowledged

	uint64_t n = produced.load();
	double enqueue_eps = n / (t1 - t0);
	double acked_eps   = n / (t2 - t0);
	double mb = (double)n * plen / (1024.0 * 1024.0);

	// CSV: backend,acks,payload,events,threads,enqueue_eps,enqueue_mbps,acked_eps,acked_mbps,delivered,errors,t_start,t_end
	// t_start/t_end bound the acked window [t0,t2] for cross-process aggregation.
	printf("%s,%s,%zu,%llu,%d,%.0f,%.2f,%.0f,%.2f,%llu,%llu,%.6f,%.6f\n",
	       sync ? "kafka-sync" : "kafka",
	       acks, plen, (unsigned long long)n, threads,
	       enqueue_eps, enqueue_eps * plen / (1024*1024),
	       acked_eps,   acked_eps   * plen / (1024*1024),
	       (unsigned long long)g_delivered.load(),
	       (unsigned long long)g_errors.load(), t0, t2);
	fprintf(stderr, "[%s] size=%zu thr=%d acks=%s | enqueue %.0f ev/s (%.1f MB/s) | "
		"acked %.0f ev/s (%.1f MB/s) | delivered=%llu errors=%llu | %.1f MB in %.2fs\n",
		sync ? "kafka-sync" : "kafka",
		plen, threads, acks, enqueue_eps, enqueue_eps*plen/(1024*1024),
		acked_eps, acked_eps*plen/(1024*1024),
		(unsigned long long)g_delivered.load(),
		(unsigned long long)g_errors.load(), mb, t2 - t0);

	rd_kafka_topic_destroy(rkt);
	rd_kafka_destroy(rk);
	return 0;
}
