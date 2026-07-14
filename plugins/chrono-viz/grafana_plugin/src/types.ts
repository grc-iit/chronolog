import { DataQuery, DataSourceJsonData } from '@grafana/data';

// ============================================================================
// Grafana Plugin Types
// ============================================================================

/**
 * How events are fetched from ChronoLog (exclusive choice):
 * - 'replay':   archive read over the dashboard time range (Client.ReplayStory)
 * - 'playback': tail read of the most recent N events from keeper memory
 *               (StoryHandle.playback); the dashboard time range is ignored
 */
export type ChronoLogQueryType = 'replay' | 'playback';

/** Default number of events fetched in playback (tail read) mode */
export const DEFAULT_NUM_EVENTS = 100;

/**
 * ChronoLog query interface for Grafana
 */
export interface ChronoLogQuery extends DataQuery {
  /** Name of the chronicle to query */
  chronicleName: string;
  /** Name of the story within the chronicle */
  storyName: string;
  /** Fetch mode; defaults to 'replay' when unset (pre-existing queries) */
  queryType?: ChronoLogQueryType;
  /** Number of most recent events to fetch in 'playback' mode */
  numEvents?: number;
  /** Query text for filtering (optional) */
  queryText?: string;
}

/**
 * Default query values
 */
export const DEFAULT_QUERY: Partial<ChronoLogQuery> = {
  chronicleName: '',
  storyName: '',
  queryType: 'replay',
  numEvents: DEFAULT_NUM_EVENTS,
};

/**
 * Configuration options for the ChronoLog data source
 */
export interface ChronoLogDataSourceOptions extends DataSourceJsonData {
  /** URL of the ChronoLog backend service */
  backendUrl: string;
  /** Default chronicle name for new queries (optional) */
  defaultChronicleName?: string;
  /** Default story name for new queries (optional) */
  defaultStoryName?: string;
  /** Path to ChronoLog client config JSON on the backend host (optional, default: install_dir/chronolog/conf/default_client_conf.json) */
  clientConfFile?: string;
}

/**
 * Secure JSON data (not visible in UI after save)
 */
export interface ChronoLogSecureJsonData {
  /** API key for authentication (if needed) */
  apiKey?: string;
}

// ============================================================================
// Request Types (matching backend Pydantic models)
// ============================================================================

/**
 * Request model for connecting to ChronoLog service
 * Matches ConnectionConfig in chronolog_service.py
 */
export interface ConnectRequest {
  protocol?: string;
  portal_ip?: string;
  portal_port?: number;
  portal_provider_id?: number;
  query_ip?: string;
  query_port?: number;
  query_provider_id?: number;
  /** Path to client config JSON on the backend host */
  conf_file?: string;
}

/**
 * Request model for querying story events
 * Matches QueryRequest in chronolog_service.py
 */
export interface QueryRequest {
  chronicle_name: string;
  story_name: string;
  start_time: number;
  end_time: number;
}

/**
 * Request model for tail-reading the most recent story events
 * Matches PlaybackRequest in chronolog_service.py
 */
export interface PlaybackRequest {
  chronicle_name: string;
  story_name: string;
  num_events: number;
}

// ============================================================================
// Response Types (matching backend Pydantic models)
// ============================================================================

/**
 * Response from the health endpoint
 * Matches HealthResponse in chronolog_service.py
 */
export interface HealthResponse {
  status: string;
  chronolog_available: boolean;
  connected: boolean;
}

/**
 * Response from connect endpoint
 * Matches ConnectionResponse in chronolog_service.py
 */
export interface ConnectResponse {
  success: boolean;
  message: string;
}

/**
 * Response from disconnect endpoint
 * Matches DisconnectionResponse in chronolog_service.py
 */
export interface DisconnectResponse {
  success: boolean;
  message: string;
}

/**
 * ChronoLog Event structure matching the backend EventResponse model
 */
export interface ChronoLogEvent {
  /** Event timestamp (nanoseconds since epoch) */
  time: number;
  /** Client ID that logged the event */
  client_id: number;
  /** Event index within the client */
  index: number;
  /** Log record content */
  log_record: string;
}

/**
 * Response from the query endpoint
 * Matches QueryResponse in chronolog_service.py
 */
export interface QueryResponse {
  chronicle_name: string;
  story_name: string;
  start_time: number;
  end_time: number;
  events: ChronoLogEvent[];
  total_events: number;
}

/**
 * Response from the playback endpoint
 * Matches PlaybackResponse in chronolog_service.py
 */
export interface PlaybackResponse {
  chronicle_name: string;
  story_name: string;
  num_events: number;
  events: ChronoLogEvent[];
  total_events: number;
}
