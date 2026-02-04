import {
  DataQueryRequest,
  DataQueryResponse,
  DataSourceApi,
  DataSourceInstanceSettings,
  MutableDataFrame,
  FieldType,
} from '@grafana/data';
import { getBackendSrv, getTemplateSrv } from '@grafana/runtime';
import { lastValueFrom } from 'rxjs';

import {
  ChronoLogQuery,
  ChronoLogDataSourceOptions,
  QueryResponse,
  HealthResponse,
} from './types';

/**
 * ChronoLog Data Source implementation
 *
 * Communicates with the ChronoLog Python backend service to fetch
 * log events from chronicles and stories.
 */
export class DataSource extends DataSourceApi<ChronoLogQuery, ChronoLogDataSourceOptions> {
  /** Backend service URL */
  private readonly backendUrl: string;
  /** Instance settings for accessing configuration */
  private readonly instanceSettings: DataSourceInstanceSettings<ChronoLogDataSourceOptions>;

  constructor(instanceSettings: DataSourceInstanceSettings<ChronoLogDataSourceOptions>) {
    super(instanceSettings);
    this.instanceSettings = instanceSettings;
    this.backendUrl = instanceSettings.jsonData.backendUrl || 'http://localhost:8080';
  }

  /**
   * Get default query values from datasource configuration
   */
  getDefaultQuery(): Partial<ChronoLogQuery> {
    return {
      chronicleName: this.instanceSettings.jsonData.defaultChronicleName || '',
      storyName: this.instanceSettings.jsonData.defaultStoryName || '',
    };
  }

  /**
   * Execute queries for all targets
   */
  async query(options: DataQueryRequest<ChronoLogQuery>): Promise<DataQueryResponse> {
    const { range } = options;
    const from = range.from.valueOf();
    const to = range.to.valueOf();

    // Convert milliseconds to nanoseconds for ChronoLog
    const startTimeNs = from * 1_000_000;
    const endTimeNs = to * 1_000_000;

    const promises = options.targets
      .filter((target) => !target.hide)
      .map(async (target) => {
        // Apply template variable substitution
        const chronicleName = getTemplateSrv().replace(target.chronicleName || '', options.scopedVars);
        const storyName = getTemplateSrv().replace(target.storyName || '', options.scopedVars);

        if (!chronicleName || !storyName) {
          return new MutableDataFrame({
            refId: target.refId,
            fields: [],
          });
        }

        try {
          const response = await this.queryStory(chronicleName, storyName, startTimeNs, endTimeNs);
          return this.transformResponse(response, target);
        } catch (error) {
          console.error(`Error querying ${chronicleName}/${storyName}:`, error);
          return new MutableDataFrame({
            refId: target.refId,
            fields: [],
          });
        }
      });

    const data = await Promise.all(promises);
    return { data };
  }

  /**
   * Query a specific story from the backend
   * Matches backend API: POST /query with QueryRequest
   * Backend handles acquire/release lifecycle automatically
   */
  private async queryStory(
    chronicleName: string,
    storyName: string,
    startTime: number,
    endTime: number
  ): Promise<QueryResponse> {
    try {
      const response = await lastValueFrom(
        getBackendSrv().fetch<QueryResponse>({
          url: `${this.backendUrl}/query`,
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
          },
          data: {
            chronicle_name: chronicleName,
            story_name: storyName,
            start_time: startTime, // nanoseconds
            end_time: endTime, // nanoseconds
          },
        })
      );

      return response.data;
    } catch (error: any) {
      // Handle 503 Service Unavailable (client not connected)
      if (error?.status === 503) {
        throw new Error('ChronoLog client not connected. Ensure backend is connected to ChronoLog services.');
      }
      // Handle 422 Validation Error
      if (error?.status === 422) {
        throw new Error('Invalid query parameters. Check chronicle_name, story_name, start_time, and end_time.');
      }
      // Re-throw other errors
      throw error;
    }
  }

  /**
   * Transform ChronoLog events to Grafana DataFrame format
   */
  private transformResponse(response: QueryResponse, target: ChronoLogQuery): MutableDataFrame {
    const frame = new MutableDataFrame({
      refId: target.refId,
      name: `${response.chronicle_name}/${response.story_name}`,
      fields: [
        {
          name: 'Time',
          type: FieldType.time,
          values: [],
        },
        {
          name: 'Log',
          type: FieldType.string,
          values: [],
        },
        {
          name: 'Client ID',
          type: FieldType.number,
          values: [],
        },
        {
          name: 'Index',
          type: FieldType.number,
          values: [],
        },
      ],
    });

    for (const event of response.events) {
      // Convert nanoseconds to milliseconds for Grafana
      const timeMs = Math.floor(event.time / 1_000_000);
      frame.appendRow([timeMs, event.log_record, event.client_id, event.index]);
    }

    return frame;
  }

  /**
   * Test data source connectivity
   * Matches backend API: GET /health and POST /connect
   */
  async testDatasource(): Promise<{ status: string; message: string }> {
    try {
      // First check if the backend is reachable
      const healthResponse = await lastValueFrom(
        getBackendSrv().fetch<HealthResponse>({
          url: `${this.backendUrl}/health`,
          method: 'GET',
        })
      );

      const health = healthResponse.data;

      if (health.status !== 'healthy') {
        return {
          status: 'error',
          message: 'Backend service is not healthy',
        };
      }

      if (!health.chronolog_available) {
        return {
          status: 'warning',
          message: 'Backend is running but ChronoLog bindings are not available',
        };
      }

      // Try to connect if not already connected
      // Backend /connect endpoint accepts optional ConnectionConfig or uses environment defaults
      if (!health.connected) {
        try {
          const connectResponse = await lastValueFrom(
            getBackendSrv().fetch<{ success: boolean; message: string }>({
              url: `${this.backendUrl}/connect`,
              method: 'POST',
              headers: {
                'Content-Type': 'application/json',
              },
              // Send empty object - backend will use environment variable defaults
              data: {},
            })
          );

          if (!connectResponse.data.success) {
            return {
              status: 'warning',
              message: `Backend running but connection failed: ${connectResponse.data.message}`,
            };
          }
        } catch (connectError: any) {
          // Handle 503 Service Unavailable (client not connected)
          if (connectError?.status === 503) {
            return {
              status: 'warning',
              message: 'Backend running but could not connect to ChronoLog service. Check ChronoLog services are running.',
            };
          }
          return {
            status: 'warning',
            message: `Backend running but connection failed: ${connectError instanceof Error ? connectError.message : String(connectError)}`,
          };
        }
      }

      return {
        status: 'success',
        message: 'Successfully connected to ChronoLog backend service',
      };
    } catch (error: any) {
      console.error('Connection test failed:', error);
      
      // Handle specific HTTP errors
      if (error?.status === 503) {
        return {
          status: 'error',
          message: `Backend service unavailable (503). Ensure ChronoLog services are running.`,
        };
      }
      
      return {
        status: 'error',
        message: `Failed to connect to backend at ${this.backendUrl}: ${error instanceof Error ? error.message : String(error)}`,
      };
    }
  }

  /**
   * Get list of available chronicles
   * Note: Backend endpoint not yet implemented - returns empty list
   */
  async getChronicles(): Promise<string[]> {
    try {
      const response = await lastValueFrom(
        getBackendSrv().fetch<{ chronicles: string[] }>({
          url: `${this.backendUrl}/chronicles`,
          method: 'GET',
        })
      );
      return response.data?.chronicles || [];
    } catch (error: any) {
      if (error?.status === 404 || error?.status === 501) {
        console.warn('Chronicles endpoint not yet implemented in backend');
        return [];
      }
      console.error('Failed to fetch chronicles:', error);
      return [];
    }
  }

  /**
   * Get list of stories for a chronicle
   * Note: Backend endpoint not yet implemented - returns empty list
   */
  async getStories(chronicleName: string): Promise<string[]> {
    if (!chronicleName) {
      return [];
    }

    try {
      const response = await lastValueFrom(
        getBackendSrv().fetch<{ chronicle_name: string; stories: string[] }>({
          url: `${this.backendUrl}/stories/${encodeURIComponent(chronicleName)}`,
          method: 'GET',
        })
      );
      return response.data?.stories || [];
    } catch (error: any) {
      if (error?.status === 404 || error?.status === 501) {
        console.warn(`Stories endpoint not yet implemented in backend for chronicle: ${chronicleName}`);
        return [];
      }
      console.error(`Failed to fetch stories for ${chronicleName}:`, error);
      return [];
    }
  }

  /**
   * Support for Grafana annotations
   */
  async annotationQuery(options: any): Promise<any[]> {
    // Annotations can be implemented by querying for specific events
    // For now, return empty array
    return [];
  }

  /**
   * Support for Grafana template variable queries
   * Uses backend /search endpoint (currently returns empty list)
   * Falls back to /chronicles and /stories endpoints when available
   */
  async metricFindQuery(query: string): Promise<Array<{ text: string; value: string }>> {
    // Parse query to determine what to return
    const queryLower = query.toLowerCase().trim();

    // Try to use search endpoint first (for Grafana variable queries)
    try {
      const searchResponse = await lastValueFrom(
        getBackendSrv().fetch<Array<{ text: string; value: string }>>({
          url: `${this.backendUrl}/search`,
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
          },
          data: { target: query },
        })
      );
      
      // If search returns results, use them
      if (searchResponse.data && searchResponse.data.length > 0) {
        return searchResponse.data;
      }
    } catch (error) {
      // Search endpoint may not be fully implemented - fall through to chronicles/stories
      console.debug('Search endpoint not available, falling back to chronicles/stories');
    }

    // Fallback to chronicles/stories endpoints
    if (queryLower === 'chronicles' || queryLower === '*') {
      const chronicles = await this.getChronicles();
      return chronicles.map((c) => ({ text: c, value: c }));
    }

    // If query looks like "stories(chronicle_name)", return stories for that chronicle
    const storiesMatch = queryLower.match(/^stories\((.+)\)$/);
    if (storiesMatch) {
      const chronicleName = storiesMatch[1].trim();
      const stories = await this.getStories(chronicleName);
      return stories.map((s) => ({ text: s, value: s }));
    }

    return [];
  }
}

