import {DataQuery, DataSourceJsonData} from '@grafana/data';
export interface Query extends DataQuery {
    chronicle: string;
    story: string;
    fields?: string[];
    limit?: number;
    live?: boolean;
}
export interface Options extends DataSourceJsonData {}
export interface Response {
    columns: Array<{name: string; type: string}>;
    rows: unknown[][];
    meta: {complete: boolean|null; reason: string | null; frontier?: unknown; laggards: unknown[]; limited: boolean};
}
