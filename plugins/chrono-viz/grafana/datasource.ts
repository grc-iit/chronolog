import {
    DataQueryRequest,
    DataQueryResponse,
    DataSourceApi,
    DataSourceInstanceSettings,
    LoadingState
} from '@grafana/data';
import {getBackendSrv, getTemplateSrv} from '@grafana/runtime';
import {Observable, forkJoin, from, map, merge} from 'rxjs';
import {Query, Options, Response} from './types';
import {frames} from './frames';

export class DataSource extends DataSourceApi<Query, Options>
{
    readonly url: string;
    constructor(settings: DataSourceInstanceSettings<Options>)
    {
        super(settings);
        this.url = `/api/datasources/proxy/uid/${settings.uid}`;
    }
    async choices(chronicle?: string): Promise<string[]>
    {
        const response = await getBackendSrv().get(
                `${this.url}/stories${chronicle ? '?chronicle=' + encodeURIComponent(chronicle) : ''}`);
        return chronicle ? response.stories : response.chronicles;
    }
    query(request: DataQueryRequest<Query>): Observable<DataQueryResponse>
    {
        const targets = request.targets.filter(q => !q.hide && q.chronicle && q.story)
                                .map(q => ({
                                         ...q,
                                         chronicle: getTemplateSrv().replace(q.chronicle, request.scopedVars),
                                         story: getTemplateSrv().replace(q.story, request.scopedVars),
                                     }));
        if(!targets.length)
        {
            return from(Promise.resolve({data: []}));
        }
        const streams = targets.map(q => {
            const limit = Math.min(10000, Math.max(1, q.limit || 10000));
            const fields = q.fields || [];
            if(q.live)
            {
                return new Observable<DataQueryResponse>(subscriber => {
                    const params = new URLSearchParams({chronicle: q.chronicle, story: q.story});
                    const source = new EventSource(`${this.url}/tail?${params}`);
                    const rows: unknown[][] = [];
                    subscriber.next({data: [], state: LoadingState.Streaming, key: q.refId});
                    source.onmessage = event => {
                        try
                        {
                            const data = JSON.parse(event.data);
                            let payload: Record<string, unknown> = {};
                            try
                            {
                                payload = JSON.parse(data.payload);
                            }
                            catch
                            { /* Non JSON events have empty values. */
                            }
                            rows.push([
                                data.hlc.physical_ns / 1000000,
                                Object.values(data.event_id).join(':'),
                                ...fields.map(f => payload?.[f] ?? null),
                                data.labels
                            ]);
                            if(rows.length > limit)
                            {
                                rows.shift();
                            }
                            const response: Response = {
                                columns: [
                                    {name: 'time', type: 'time'},
                                    {name: 'event_id', type: 'string'},
                                    ...fields.map((f, i) => ({
                                                      name: f,
                                                      type: typeof rows[rows.length - 1][i + 2] === 'number' ? 'number'
                                                                                                             : 'string'
                                                  })),
                                    {name: 'labels', type: 'other'}
                                ],
                                rows,
                                meta: {complete: null, reason: null, laggards: [], limited: false},
                            };
                            subscriber.next(
                                    {data: frames(response, q.refId), state: LoadingState.Streaming, key: q.refId});
                        }
                        catch(error)
                        {
                            subscriber.error(error);
                        }
                    };
                    source.addEventListener('error', event => {
                        if(event instanceof MessageEvent)
                        {
                            subscriber.error(new Error(event.data));
                        }
                    });
                    return () => source.close();
                });
            }
            return getBackendSrv()
                    .fetch<Response>({
                        url: `${this.url}/query`,
                        method: 'POST',
                        data: {
                            chronicle: q.chronicle,
                            story: q.story,
                            from_ns: request.range.from.valueOf() * 1000000,
                            to_ns: request.range.to.valueOf() * 1000000,
                            fields,
                            limit,
                        }
                    })
                    .pipe(map(r => ({data: frames(r.data, q.refId), key: q.refId})));
        });
        return targets.some(q => q.live)
                       ? merge(...streams)
                       : forkJoin(streams).pipe(map(responses => ({data: responses.flatMap(r => r.data)})));
    }
    async testDatasource()
    {
        try
        {
            const result = await getBackendSrv().get(`${this.url}/health`);
            return {
                status: result.status === 'healthy' ? 'success' : 'error',
                message: 'ChronoLog Visor and Player reachable'
            };
        }
        catch(error)
        {
            return {status: 'error', message: String(error)};
        }
    }
}
