import {FieldType, MutableDataFrame} from '@grafana/data';
import {Response} from './types';

export function frames(response: Response, refId: string): MutableDataFrame[]
{
    const notices = response.meta.limited
                            ? [{severity: 'info' as const, text: 'Result limited; completeness unavailable'}]
                    : response.meta.complete === false ? [{
                          severity: 'warning' as const,
                          text: `${response.meta.reason}; laggards: ${JSON.stringify(response.meta.laggards)}`
                      }]
                                                       : [];
    const labelIndex = response.columns.findIndex(c => c.name === 'labels');
    const groups = new Map<string, unknown[][]>();
    for(const row of response.rows)
    {
        const key = labelIndex < 0 ? '{}' : JSON.stringify(row[labelIndex] || {});
        if(!groups.has(key))
        {
            groups.set(key, []);
        }
        groups.get(key)!.push(row);
    }
    if(!groups.size)
    {
        groups.set('{}', []);
    }
    return Array.from(
            groups,
            ([labels, rows]) => new MutableDataFrame({
                refId,
                fields: response.columns.filter(c => c.name !== 'labels')
                                .map(c => ({
                                         name: c.name,
                                         type: c.type as FieldType,
                                         labels: c.name !== 'time' && c.name !== 'event_id' ? JSON.parse(labels)
                                                                                            : undefined,
                                         values: rows.map(row => row[response.columns.indexOf(c)]),
                                     })),
                meta: {notices, custom: response.meta},
            }));
}
