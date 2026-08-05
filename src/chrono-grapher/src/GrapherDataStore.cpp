#include <ArchiveManifest.h>
#include <unistd.h>
#include <map>
#include <mutex>
#include <chrono>
#include <utility>
#include <deque>
#include <unordered_set>
#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <GrapherDataStore.h>
#include <GrapherExtractionChain.h>
#include <StoryWatermarkRegistry.h>
#include <WatermarkReportPublisher.h>
#include <chrono_monitor.h>

namespace chl = chronolog;
namespace tl = thallium;


////////////////////////

int chronolog::GrapherDataStore::startStoryRecording(std::string const& chronicle,
                                                     std::string const& story,
                                                     chronolog::StoryId const& story_id,
                                                     uint64_t start_time,
                                                     bool is_adoption)
{
    LOG_INFO("[GrapherDataStore] Start recording story: Chronicle={}, Story={}, StoryId={}",
             chronicle,
             story,
             story_id);

    // Get dataStoreMutex, check for story_id_presence & add new StoryPipeline if needed
    std::lock_guard storeLock(dataStoreMutex);

    if(is_adoption)
    {
        // Recovery must not resurrect a destroyed story/chronicle: destroyStory/
        // destroyChronicle deletes its HDF5 files under this same lock, and a
        // pipeline created here would archive a fresh file the destroy worker can
        // no longer clean up. Refuse; the caller discards the chunk. (Checking and
        // creating under one lock makes this atomic w.r.t. destroy: if destroy ran
        // first the tombstone is set and we refuse; if we create first the pipeline
        // is in the map and destroy's own scan finds and deletes it.)
        if(destroyedStories.find(story_id) != destroyedStories.end() ||
           destroyedChronicles.find(chronicle) != destroyedChronicles.end())
        {
            LOG_INFO("[GrapherDataStore] Refusing to adopt orphan chunk for destroyed StoryId={} (Chronicle={})",
                     story_id,
                     chronicle);
            return chronolog::CL_ERR_UNKNOWN;
        }
    }
    else
    {
        // A real (visor-driven) registration means the story/chronicle is alive
        // again; drop any tombstone so future recovery for it is allowed.
        destroyedStories.erase(story_id);
        destroyedChronicles.erase(chronicle);
    }

    auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
    if(pipeline_iter != theMapOfStoryPipelines.end())
    {
        LOG_INFO("[GrapherDataStore] Story already being recorded. StoryId: {}", story_id);
        //check it the pipeline was put on the waitingForExit list by the previous acquisition
        // and remove it from there
        auto waiting_iter = pipelinesWaitingForExit.find(story_id);
        if(waiting_iter != pipelinesWaitingForExit.end())
        {
            pipelinesWaitingForExit.erase(waiting_iter);
        }

        if(theWatermarkRegistry != nullptr)
        {
            // live pipeline: its open windows may hold unpersisted events, so
            // the registry must not treat [W, start_time) as covered
            theWatermarkRegistry->registerStory(story_id, start_time, /*fresh_pipeline=*/false);
        }
        return chronolog::CL_SUCCESS;
    }

    auto result = theMapOfStoryPipelines.emplace(
            std::pair<chl::StoryId, chl::StoryPipeline*>(story_id,
                                                         new chl::StoryPipeline(chronicle,
                                                                                story,
                                                                                story_id,
                                                                                start_time,
                                                                                story_chunk_duration_secs,
                                                                                acceptance_window_secs)));

    if(result.second)
    {
        LOG_INFO("[GrapherDataStore] New StoryPipeline created successfully. StoryId {}", story_id);
        pipeline_iter = result.first;
        // give the pipeline the extraction queue as the escape hatch for
        // events whose timeline window can no longer be re-opened
        (*pipeline_iter).second->attachExtractionQueue(&theExtractionQueue);
        if(theWatermarkRegistry != nullptr)
        {
            // adoption is a recovery path for chunks that already exist below
            // start_time — never let it cover the gap up to start_time
            theWatermarkRegistry->registerStory(story_id, start_time, /*fresh_pipeline=*/!is_adoption);
        }
        //engage StoryPipeline with the IngestionQueue
        chl::StoryChunkIngestionHandle* ingestionHandle = (*pipeline_iter).second->getActiveIngestionHandle();
        theIngestionQueue.addStoryIngestionHandle(story_id, ingestionHandle);
        return chronolog::CL_SUCCESS;
    }
    else
    {
        LOG_ERROR(
                "[GrapherDataStore] Failed to create StoryPipeline for StoryId: {}. Possible memory or resource issue.",
                story_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
}
////////////////////////

int chronolog::GrapherDataStore::stopStoryRecording(chronolog::StoryId const& story_id)
{
    LOG_DEBUG("[GrapherDataStore] Initiating stop recording for StoryId={}", story_id);
    // we do not yet disengage the StoryPipeline from the IngestionQueue right away
    // but put it on the WaitingForExit list to be finalized, persisted to disk , and
    // removed from memory at exit_time = now+acceptance_window...
    // unless there's a new story acquisition request comes before that moment
    std::lock_guard storeLock(dataStoreMutex);
    auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
    if(pipeline_iter != theMapOfStoryPipelines.end())
    {
        uint64_t exit_time = std::chrono::high_resolution_clock::now().time_since_epoch().count() +
                             inactive_pipeline_delay_secs * 1000000000;
        // (*pipeline_iter).second->getAcceptanceWindow();
        pipelinesWaitingForExit[(*pipeline_iter).first] =
                (std::pair<chl::StoryPipeline*, uint64_t>((*pipeline_iter).second, exit_time));
        LOG_INFO("[GrapherDataStore] Scheduled pipeline to retire: StoryId {} timeline {}-{} acceptanceWindow {} "
                 "retirementTime {}",
                 (*pipeline_iter).second->getStoryId(),
                 (*pipeline_iter).second->TimelineStart(),
                 (*pipeline_iter).second->TimelineEnd(),
                 (*pipeline_iter).second->getAcceptanceWindow(),
                 exit_time);
    }
    else
    {
        LOG_WARNING("[GrapherDataStore] Attempt to stop recording for non-existent StoryId={}", story_id);
    }
    return chronolog::CL_SUCCESS;
}

////////////////////////

int chronolog::GrapherDataStore::destroyStory(chronolog::StoryId const& story_id,
                                              chronolog::ChronicleName const& chronicle,
                                              chronolog::StoryName const& story)
{
    LOG_INFO("[GrapherDataStore] Destroying story (async): Chronicle={}, Story={}, StoryId={}",
             chronicle,
             story,
             story_id);

    DestroyTask task;
    task.kind = DestroyTask::Kind::Story;
    task.chronicleName = chronicle;
    task.storyName = story;

    // Steps 1-3: under dataStoreMutex, unhook the pipeline from both maps,
    // absorb in-flight chunks, and unhook the ingestion handle. All three
    // must be atomic w.r.t. startStoryRecording: story_id is a deterministic
    // CityHash64(chronicle+story), so a Destroy-then-recreate-then-reAcquire
    // can reach this same Grapher while this destroy is in flight. If we
    // released dataStoreMutex before removeStoryIngestionHandle, a concurrent
    // startStoryRecording could install a fresh handle in the gap and we
    // would erroneously remove it -- orphaning every subsequent event for
    // the new acquisition (silent data loss). Safe to hold the lock across
    // the IngestionQueue calls: the established locking order in this file
    // is dataStoreMutex -> IngestionQueue (startStoryRecording, retire path).
    chl::StoryPipeline* pipeline = nullptr;
    {
        std::lock_guard storeLock(dataStoreMutex);
        // Tombstone the story so a late chunk cannot be adopted into a fresh
        // pipeline (and a fresh HDF5 file) after the destroy worker deletes files.
        destroyedStories.insert(story_id);
        auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
        if(pipeline_iter != theMapOfStoryPipelines.end())
        {
            pipeline = (*pipeline_iter).second;
            theMapOfStoryPipelines.erase(pipeline_iter);
            pipelinesWaitingForExit.erase(story_id);
        }
        else
        {
            auto waiting_iter = pipelinesWaitingForExit.find(story_id);
            if(waiting_iter != pipelinesWaitingForExit.end())
            {
                pipeline = waiting_iter->second.first;
                pipelinesWaitingForExit.erase(waiting_iter);
            }
        }

        if(pipeline != nullptr)
        {
            // drainOrphanChunks is global to the ingestion queue (cheap) and
            // pipeline->collectIngestedEvents folds both ingestion deques into
            // the pipeline timeline before we unhook the handle so the worker
            // still persists everything that arrived before Destroy.
            theIngestionQueue.drainOrphanChunks();
            pipeline->collectIngestedEvents();
            theIngestionQueue.removeStoryIngestionHandle(story_id);
        }
    }

    if(pipeline != nullptr)
    {
        // Finalize outside the lock; the pipeline is already unreachable via
        // any map or ingestion handle, so no other thread can touch it.
        pipeline->finalize(task.remainingChunks);
        task.pipelines.push_back(pipeline);
    }

    // Step 5/6: hand ownership over to the destroy worker. Ownership of
    // task.pipelines and task.remainingChunks transfers to the worker; the
    // worker deletes both after the wait completes.
    enqueueDestroyTask(std::move(task));
    return chronolog::CL_SUCCESS;
}

////////////////////////

int chronolog::GrapherDataStore::destroyChronicle(chronolog::ChronicleName const& chronicle)
{
    LOG_INFO("[GrapherDataStore] Destroying chronicle (async): {}", chronicle);

    DestroyTask task;
    task.kind = DestroyTask::Kind::Chronicle;
    task.chronicleName = chronicle;

    // Under dataStoreMutex, unhook every matching pipeline, drain/absorb
    // their in-flight chunks, and unhook their ingestion handles -- all in
    // one critical section. See destroyStory above for the rationale: any
    // gap between the map erase and the handle remove lets a concurrent
    // startStoryRecording for a freshly recreated story slip in and have
    // its new ingestion handle erroneously removed (silent data loss).
    std::vector<chl::StoryId> story_ids_to_unhook;
    {
        std::lock_guard storeLock(dataStoreMutex);
        // Tombstone the chronicle so late chunks for any of its stories (including
        // ones already retired from the maps) cannot be adopted into fresh HDF5
        // files after the destroy worker deletes them.
        destroyedChronicles.insert(chronicle);
        for(auto it = theMapOfStoryPipelines.begin(); it != theMapOfStoryPipelines.end();)
        {
            if(it->second != nullptr && it->second->getChronicleName() == chronicle)
            {
                story_ids_to_unhook.push_back(it->first);
                task.pipelines.push_back(it->second);
                pipelinesWaitingForExit.erase(it->first);
                it = theMapOfStoryPipelines.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for(auto it = pipelinesWaitingForExit.begin(); it != pipelinesWaitingForExit.end();)
        {
            chl::StoryPipeline* p = it->second.first;
            if(p != nullptr && p->getChronicleName() == chronicle)
            {
                story_ids_to_unhook.push_back(it->first);
                task.pipelines.push_back(p);
                it = pipelinesWaitingForExit.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if(!task.pipelines.empty())
        {
            theIngestionQueue.drainOrphanChunks();
            for(chl::StoryPipeline* p: task.pipelines)
            {
                if(p != nullptr)
                {
                    p->collectIngestedEvents();
                }
            }
            for(chl::StoryId const& sid: story_ids_to_unhook) { theIngestionQueue.removeStoryIngestionHandle(sid); }
        }
    }

    // Finalize outside the lock; each pipeline is already unreachable via
    // any map or ingestion handle, so no other thread can touch it.
    for(chl::StoryPipeline* p: task.pipelines)
    {
        if(p != nullptr)
        {
            p->finalize(task.remainingChunks);
        }
    }

    enqueueDestroyTask(std::move(task));
    return chronolog::CL_SUCCESS;
}

////////////////////////

void chronolog::GrapherDataStore::enqueueDestroyTask(DestroyTask&& task)
{
    {
        std::lock_guard<std::mutex> lock(destroyMutex);
        destroyQueue.emplace_back(std::move(task));
    }
    destroyCv.notify_one();
}

////////////////////////

void chronolog::GrapherDataStore::drainExtractionQueueOrTimeout()
{
    // Poll the extraction queue until it drains. Destroy is rare; per-story
    // granularity would be nicer but a global drain wait is the simplest
    // robust primitive the queue currently exposes. To avoid blocking
    // shutdown indefinitely we also bail when the data store is shutting
    // down -- the extraction module's own shutdown will then take over.
    using namespace std::chrono_literals;
    auto const poll_interval = 50ms;
    while(!destroyWorkerShouldExit.load(std::memory_order_acquire))
    {
        if(theExtractionQueue.empty())
        {
            return;
        }
        std::this_thread::sleep_for(poll_interval);
    }
}

////////////////////////

void chronolog::GrapherDataStore::destroyWorkerTask()
{
    LOG_INFO("[GrapherDataStore] Destroy worker thread started");
    while(true)
    {
        DestroyTask task;
        {
            std::unique_lock<std::mutex> lock(destroyMutex);
            destroyCv.wait(lock,
                           [this] {
                               return destroyWorkerShouldExit.load(std::memory_order_acquire) || !destroyQueue.empty();
                           });
            if(destroyQueue.empty() && destroyWorkerShouldExit.load(std::memory_order_acquire))
            {
                break;
            }
            task = std::move(destroyQueue.front());
            destroyQueue.pop_front();
        }

        // Hand the drained chunks to the extraction queue so the existing
        // writer threads persist them via the configured chain (e.g. HDF5).
        // This must happen BEFORE we wait for drain; otherwise the wait
        // would return immediately and we'd delete files before they were
        // written.
        if(!task.remainingChunks.empty())
        {
            theExtractionQueue.stashStoryChunks(task.remainingChunks);
            task.remainingChunks.clear();
        }

        // Wait for the extraction queue to drain. This is the load-bearing
        // ordering primitive: by the time we proceed to delete files, every
        // chunk we stashed (and anything stashed concurrently for other
        // stories) has been picked up by a writer thread.
        drainExtractionQueueOrTimeout();

        // Persistence-vs-deletion ordering is now safe; delete the on-disk
        // HDF5 artifacts directly via the extraction chain.
        int delete_rc = chronolog::CL_SUCCESS;
        if(theExtractionChain != nullptr)
        {
            if(task.kind == DestroyTask::Kind::Story)
            {
                delete_rc = theExtractionChain->delete_story_files(task.chronicleName, task.storyName);
                LOG_INFO("[GrapherDataStore] Destroy worker: delete_story_files Chronicle={}, Story={} rc={}",
                         task.chronicleName,
                         task.storyName,
                         delete_rc);
            }
            else
            {
                delete_rc = theExtractionChain->delete_chronicle_files(task.chronicleName);
                LOG_INFO("[GrapherDataStore] Destroy worker: delete_chronicle_files Chronicle={} rc={}",
                         task.chronicleName,
                         delete_rc);
            }
        }
        else
        {
            LOG_WARNING("[GrapherDataStore] Destroy worker: no extraction chain configured; skipping file deletion "
                        "for Chronicle={}",
                        task.chronicleName);
        }

        // Pipelines are deleted last so that any references the extraction
        // chain might still hold to their chunks are no longer reachable.
        for(chl::StoryPipeline* p: task.pipelines) { delete p; }
        task.pipelines.clear();
    }
    LOG_INFO("[GrapherDataStore] Destroy worker thread exiting");
}

////////////////////////

void chronolog::GrapherDataStore::collectIngestedEvents()
{
    LOG_DEBUG("[GrapherDataStore] Initiating collection of ingested story chunks. Current state={}, Active "
              "StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());
    theIngestionQueue.drainOrphanChunks();

    std::lock_guard storeLock(dataStoreMutex);
    for(auto pipeline_iter = theMapOfStoryPipelines.begin(); pipeline_iter != theMapOfStoryPipelines.end();
        ++pipeline_iter)
    {
        //INNA: this can be delegated to different threads handling individual storylines...
        (*pipeline_iter).second->collectIngestedEvents();
    }
}

////////////////////////
void chronolog::GrapherDataStore::extractDecayedStoryChunks()
{
    LOG_DEBUG("[GrapherDataStore] Initiating extraction of decayed story chunks. Current state={}, Active "
              "StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());

    uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    std::vector<chl::StoryChunk*> extracted_story_chunks;
    std::lock_guard storeLock(dataStoreMutex);
    for(auto pipeline_iter = theMapOfStoryPipelines.begin(); pipeline_iter != theMapOfStoryPipelines.end();
        ++pipeline_iter)
    {
        (*pipeline_iter).second->extractDecayedStoryChunks(current_time, extracted_story_chunks);
    }
    theExtractionQueue.stashStoryChunks(extracted_story_chunks);
}
////////////////////////

void chronolog::GrapherDataStore::retireDecayedPipelines()
{
    LOG_TRACE("[GrapherDataStore] Initiating retirement of decayed pipelines. Current state={}, Active "
              "StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());

    std::vector<chl::StoryChunk*> extracted_story_chunks;

    if(!theMapOfStoryPipelines.empty())
    {
        std::lock_guard storeLock(dataStoreMutex);
        StoryPipeline* pipeline = nullptr;
        uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        for(auto pipeline_iter = pipelinesWaitingForExit.begin(); pipeline_iter != pipelinesWaitingForExit.end();)
        {
            if(current_time >= (*pipeline_iter).second.second)
            {
                //current_time >= pipeline exit_time
                pipeline = (*pipeline_iter).second.first;
                LOG_INFO("[GrapherDataStore] retiring pipeline StoryId {} timeline {}-{} acceptanceWindow {} "
                         "retirementTime {}",
                         pipeline->getStoryId(),
                         pipeline->TimelineStart(),
                         pipeline->TimelineEnd(),
                         pipeline->getAcceptanceWindow(),
                         (*pipeline_iter).second.second);
                theMapOfStoryPipelines.erase(pipeline->getStoryId());
                theIngestionQueue.removeStoryIngestionHandle(pipeline->getStoryId());
                //extract any remaining story chunks
                pipeline->finalize(extracted_story_chunks);
                pipeline_iter = pipelinesWaitingForExit.erase(pipeline_iter);
                delete pipeline;
            }
            else
            {
                pipeline_iter++;
            }
        }
    }

    theExtractionQueue.stashStoryChunks(extracted_story_chunks);

    LOG_TRACE("[GrapherDataStore] Completed retirement of decayed pipelines. Current state={}, Active "
              "StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());
}

void chronolog::GrapherDataStore::adoptOrphanChunks()
{
    // Chunks whose story has no registered pipeline (retired past its inactive
    // window, or a registration that never reached this grapher) would otherwise
    // sit in the ingestion orphan queue and eventually be dropped. Unlike a raw
    // event, a StoryChunk is self-describing -- it carries its chronicle/story
    // names -- so we recover it: create a pipeline on-demand from the chunk's own
    // identity, re-ingest the chunk for normal sealing/archival, and schedule the
    // on-demand pipeline to retire so it does not leak.
    std::deque<chl::StoryChunk*> orphan_chunks = theIngestionQueue.extractOrphanChunks();
    if(orphan_chunks.empty())
    {
        return;
    }

    std::unordered_set<chl::StoryId> adopted_stories;
    std::size_t discarded = 0;
    for(chl::StoryChunk* chunk: orphan_chunks)
    {
        // startStoryRecording(is_adoption=true) reuses an existing pipeline, or
        // creates one from the chunk's own names -- unless the story/chronicle is
        // tombstoned as destroyed, in which case it refuses and we discard the
        // chunk rather than resurrecting a deleted archive.
        int rc = startStoryRecording(chunk->getChronicleName(),
                                     chunk->getStoryName(),
                                     chunk->getStoryId(),
                                     chunk->getStartTime(),
                                     /*is_adoption=*/true);
        if(rc != chronolog::CL_SUCCESS)
        {
            delete chunk;
            ++discarded;
            continue;
        }
        theIngestionQueue.ingestStoryChunk(chunk);
        adopted_stories.insert(chunk->getStoryId());
    }

    // Schedule each on-demand pipeline to retire (now + inactive_pipeline_delay)
    // so it is not left resident forever. A subsequent real acquisition rescues
    // it from the waiting list; further stragglers re-adopt and reset the timer.
    for(chl::StoryId const& story_id: adopted_stories) { stopStoryRecording(story_id); }

    LOG_WARNING("[GrapherDataStore] Adopted orphan chunk(s) for {} story(ies) from the chunks' own chronicle/story "
                "identity; discarded {} chunk(s) for destroyed stories.",
                adopted_stories.size(),
                discarded);
}

////////////////////////

void chronolog::GrapherDataStore::dataCollectionTask()
{
    //run dataCollectionTask as long as the state == RUNNING
    // or there're still events left to collect and
    // storyPipelines left to retire...
    tl::xstream es = tl::xstream::self();
    LOG_DEBUG("[GrapherDataStore] Initiating DataCollectionTask. ESrank={}, ThreadID={}",
              es.get_rank(),
              tl::thread::self_id());

    while(!is_shutting_down())
    {
        LOG_DEBUG("[GrapherDataStore] Running DataCollection iteration. ESrank={}, ThreadID={}",
                  es.get_rank(),
                  tl::thread::self_id());
        for(int i = 0; i < 1; ++i)
        {
            collectIngestedEvents();
            sleep(1);
        }
        extractDecayedStoryChunks();
        retireDecayedPipelines();
        adoptOrphanChunks();
        if(theWatermarkPublisher != nullptr)
        {
            // rate-limited inside the publisher; coalescing comes from the
            // registry's dirty snapshot
            theWatermarkPublisher->publish();
        }
        compactArchiveManifest();
    }
    LOG_DEBUG("[GrapherDataStore] Exiting DataCollectionTask thread {}", tl::thread::self_id());
}

////////////////////////

// Fold the append-only log into the snapshot once it has grown enough since the
// last compaction. Threshold-based rather than every tick because compaction
// rewrites every record: doing it each second would turn a cheap append-only log
// into a quadratic rewrite. Crash-safe either way -- the snapshot is published by
// rename, and the log is only truncated afterwards.
void chronolog::GrapherDataStore::compactArchiveManifest()
{
    if(theArchiveManifest == nullptr)
    {
        return;
    }
    std::size_t const record_count = theArchiveManifest->records().size();
    if(record_count < theRecordsAtLastCompaction + theManifestCompactionThreshold)
    {
        return;
    }
    if(theArchiveManifest->snapshot() == chronolog::CL_SUCCESS)
    {
        theRecordsAtLastCompaction = record_count;
    }
}

////////////////////////
void chronolog::GrapherDataStore::startDataCollection(int stream_count)
{
    std::lock_guard storeLock(dataStoreStateMutex);
    if(is_running() || is_shutting_down())
    {
        LOG_INFO("[GrapherDataStore] Data collection is already running or shutting down. Ignoring request.");
        return;
    }

    LOG_INFO("[GrapherDataStore] Starting data collection. StreamCount={}, ThreadID={}",
             stream_count,
             tl::thread::self_id());
    state = RUNNING;

    for(int i = 0; i < stream_count; ++i)
    {
        tl::managed<tl::xstream> es = tl::xstream::create();
        dataStoreStreams.push_back(std::move(es));
    }

    for(int i = 0; i < 2 * stream_count; ++i)
    {
        tl::managed<tl::thread> th =
                dataStoreStreams[i % (dataStoreStreams.size())]->make_thread([p = this]() { p->dataCollectionTask(); });
        dataStoreThreads.push_back(std::move(th));
    }

    // Spawn the dedicated destroy worker. We use a std::thread (not a thallium
    // ULT) so the worker can block on the extraction-queue drain via
    // condition_variable / sleep without occupying an xstream that other
    // ULTs need.
    destroyWorkerShouldExit.store(false, std::memory_order_release);
    destroyWorkerThread = std::thread([this]() { this->destroyWorkerTask(); });

    LOG_INFO("[GrapherDataStore] Data collection started successfully. Stream count={}, ThreadID={}",
             stream_count,
             tl::thread::self_id());
}
//////////////////////////////

void chronolog::GrapherDataStore::shutdownDataCollection()
{
    // switch the state to shuttingDown
    std::lock_guard storeLock(dataStoreStateMutex);
    if(is_shutting_down())
    {
        LOG_INFO("[GrapherDataStore] Data collection is already shutting down. Ignoring additional shutdown request.");
        return;
    }

    LOG_INFO("[GrapherDataStore] Initiating shutdown of DataCollection. CurrentState={}, MapOfStoryPipelines={}, "
             "PipelinesWaitingForExit={}",
             state,
             theMapOfStoryPipelines.size(),
             pipelinesWaitingForExit.size());

    state = SHUTTING_DOWN;

    // Signal the destroy worker to exit and wake it up. We join after the
    // data-collection ULTs have joined so any final destroy enqueued during
    // shutdown has a chance to be processed.
    destroyWorkerShouldExit.store(true, std::memory_order_release);
    destroyCv.notify_all();

    // Join threads & execution streams while holding stateMutex
    for(auto& th: dataStoreThreads) { th->join(); }
    LOG_INFO("[GrapherDataStore] All data collection threads have been joined.");

    if(destroyWorkerThread.joinable())
    {
        destroyWorkerThread.join();
        LOG_INFO("[GrapherDataStore] Destroy worker thread has been joined.");
    }

    for(auto& es: dataStoreStreams) { es->join(); }
    LOG_INFO("[GrapherDataStore] All data collection streams have been joined.");

    // Retire all remaining StoryPipelines
    if(!theMapOfStoryPipelines.empty())
    {
        std::lock_guard storeLock(dataStoreMutex);

        chl::StoryPipeline* pipeline = nullptr;
        std::vector<StoryChunk*> remaining_story_chunks;
        for(auto pipeline_iter = theMapOfStoryPipelines.begin(); pipeline_iter != theMapOfStoryPipelines.end();
            ++pipeline_iter)
        {
            pipeline = (*pipeline_iter).second;
            LOG_INFO("[GrapherDataStore] retiring pipeline StoryId {} timeline {}-{}",
                     pipeline->getStoryId(),
                     pipeline->TimelineStart(),
                     pipeline->TimelineEnd());
            theIngestionQueue.removeStoryIngestionHandle(pipeline->getStoryId());
            //extract any remaining story chunks
            pipeline->finalize(remaining_story_chunks);
            delete pipeline;
        }

        theExtractionQueue.stashStoryChunks(remaining_story_chunks);
        theMapOfStoryPipelines.clear();
        pipelinesWaitingForExit.clear();
    }
    LOG_INFO("[GrapherDataStore] StoryPipelines are shutdown. CurrentState={}, MapOfStoryPipelines={}, "
             "PipelinesWaitingForExit={}",
             state,
             theMapOfStoryPipelines.size(),
             pipelinesWaitingForExit.size());

    LOG_INFO("[GrapherDataStore] DataCollection shutdown completed.");
}

///////////////////////

//
chronolog::GrapherDataStore::~GrapherDataStore()
{
    LOG_TRACE("[GrapherDataStore] Destructor called. Initiating shutdown. Active StoryPipelines count={}",
              theMapOfStoryPipelines.size());
    shutdownDataCollection();
    LOG_INFO("[GrapherDataStore] Shutdown completed successfully. Active StoryPipelines count={}",
             theMapOfStoryPipelines.size());
}
