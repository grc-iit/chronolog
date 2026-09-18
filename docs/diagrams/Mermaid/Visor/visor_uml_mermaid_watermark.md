# ChronoVisor (watermark) — class interaction (UML)

Live (compiled) classes for the `watermark-feedback` branch. `direction TB`; node fill = architectural plane (see legend, including the highlighted watermark/retention/hot-fetch plane added on this branch). Mermaid `classDiagram` counterpart of the Graphviz version under `UML/Visor/`. `htmlLabels:false` for native-text SVGs that render in non-browser viewers.

```mermaid
%%{init: {"htmlLabels": false, "class": {"htmlLabels": false}}}%%
%% ChronoLog ChronoVisor (watermark-feedback branch) — class interaction (UML class diagram)
%% Relationship key:  *-- composition (owns lifetime)   o-- aggregation (holds ref/ptr)
%%                    <|-- inherits                      ..> dependency / RPC (label "RPC:")
classDiagram
  direction TB

  namespace external_processes {
    class Client:::ext {
      <<storyteller>>
    }
    class ChronoKeeper:::ext {
      <<DataStoreAdminService :7777>>
    }
    class ChronoGrapher:::ext {
      <<DataStoreAdminService :4444>>
    }
    class ChronoPlayer:::ext {
      <<PlayerStoreAdminService :2222>>
    }
  }

  class tlProvider["tl::provider&lt;T&gt;"]:::lib {
    <<Thallium / Margo>>
  }

  namespace control_plane {
    class ClientPortalService:::ctrl {
      <<tl::provider — :5555>>
      -ChronicleMetaDirectory& theMetaDirectory
      -ClientRegistryManager& theClientRegistry
      -KeeperRegistry& theKeeperRegistry
      +Connect()
      +CreateChronicle()
      +AcquireStory()
      +ReleaseStory()
      +DestroyStory()
      +Show()
    }
    class KeeperRegistryService:::ctrl {
      <<tl::provider — :8888>>
      -KeeperRegistry& theKeeperRegistry
      +register_keeper_grapher_player()
      +unregister()
      +handle_stats_msg()
    }
    class DataStoreAdminClient:::ctrl {
      <<one per process, on registryEngine>>
      -tl::provider_handle service_handle
      -tl::remote_procedure start_story_recording
      -tl::remote_procedure start_story_recording_with_keepers
      +send_start_story_recording()
      +send_start_story_recording_with_keepers(roster)
      +send_stop_story_recording()
      +destroy()
    }
  }

  namespace registry_metadata {
    class KeeperRegistry:::reg {
      -Map~GroupId, RecordingGroup~ recordingGroups
      -Map~StoryId, RecordingGroup~ activeStories
      +registerKeeper_Grapher_PlayerProcess()
      +notifyRecordingGroupOfStoryRecordingStart()
      +notifyKeepersOfStoryRecordingStart()
      +notifyGrapherOfStoryRecordingStart()
      +notifyPlayerOfStoryRecordingStart(story_keepers)
      +notifyRecordingGroupOfStoryRecordingStop()
    }
    class RecordingGroup:::reg {
      -Map~id, KeeperProcessEntry~ keeperProcesses
      -GrapherProcessEntry* grapherProcess
      -PlayerProcessEntry* playerProcess
    }
    class MetaDirectory["ChronicleMetaDirectory + ClientRegistryManager"]:::reg {
      +create_acquire_release_story()
      +add_remove_client_record()
    }
  }

  namespace common_data {
    class KeeperIdCard["KeeperIdCard / ServiceId"]:::data {
      +getRecordingServiceId()
    }
  }

  namespace legend {
    class lg_owner["A"]:::lgnd
    class lg_part["B"]:::lgnd
    class lg_whole["A"]:::lgnd
    class lg_ref["B"]:::lgnd
    class lg_base["Base"]:::lgnd
    class lg_derived["Derived"]:::lgnd
    class lg_depA["A"]:::lgnd
    class lg_depB["B"]:::lgnd
    class lg_rpcA["A"]:::lgnd
    class lg_rpcB["B"]:::lgnd
    class key_control["fill = control plane — RPC providers / admin client"]:::ctrl
    class key_registry["fill = registry / metadata — in-memory state"]:::reg
    class key_commondata["fill = common data"]:::data
    class key_external["fill = external process"]:::ext
    class key_library["fill = library base (tl::provider)"]:::lib
  }

  %% ---- generalization (inherits) ----
  tlProvider <|-- ClientPortalService
  tlProvider <|-- KeeperRegistryService

  %% ---- composition (owns lifetime) ----
  KeeperRegistry "1" *-- "many" RecordingGroup : owns recordingGroups

  %% ---- aggregation (holds ref / ptr, not owner) ----
  ClientPortalService o-- MetaDirectory : &
  ClientPortalService o-- KeeperRegistry : &
  KeeperRegistryService o-- KeeperRegistry : &
  KeeperRegistry o-- DataStoreAdminClient : * (per process)
  RecordingGroup o-- KeeperIdCard : keeper id cards

  %% ---- in-process dependency ----
  ClientPortalService ..> KeeperRegistry : AcquireStory -> notify start
  KeeperRegistry ..> DataStoreAdminClient : issue notification
  KeeperRegistry ..> KeeperIdCard : roster = getRecordingServiceId()

  %% ---- cross-process RPC ----
  Client ..> ClientPortalService : RPC ▸Connect / Acquire / Release / Show
  ChronoKeeper ..> KeeperRegistryService : RPC ▸register / stats
  ChronoGrapher ..> KeeperRegistryService : RPC ▸register / stats
  ChronoPlayer ..> KeeperRegistryService : RPC ▸register / stats
  DataStoreAdminClient ..> ChronoKeeper : RPC ▸start/stop_story_recording (4-arg)
  DataStoreAdminClient ..> ChronoGrapher : RPC ▸start/stop_story_recording (4-arg)
  DataStoreAdminClient ..> ChronoPlayer : RPC ▸start_story_recording_with_keepers (roster)

  %% ---- legend: relationship glyphs ----
  lg_owner *-- lg_part : composition ▸ owner manages lifetime
  lg_whole o-- lg_ref : aggregation ▸ holds borrowed ref / ptr
  lg_base <|-- lg_derived : generalization ▸ inherits (triangle to base)
  lg_depA ..> lg_depB : dependency ▸ uses / calls (in-process)
  lg_rpcA ..> lg_rpcB : RPC ▸ cross-process (dashed)

  %% ---- colors by plane ----
  classDef lgnd   fill:#eeeeee,stroke:#999999,color:#000
  classDef ctrl   fill:#bcd4e6,stroke:#5a7fa6,color:#000
  classDef reg    fill:#bfe3bf,stroke:#5a8a5a,color:#000
  classDef data   fill:#dcdcdc,stroke:#999999,color:#000
  classDef ext    fill:#ffe0b2,stroke:#cc8800,color:#000
  classDef lib    fill:#eeeeee,stroke:#aaaaaa,color:#000
```
