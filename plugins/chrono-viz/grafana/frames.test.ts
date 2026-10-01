import {frames} from './frames';
import {Response} from './types';
test('maps time, EventId, values, labels and authoritative notices', () => {
    const response: Response = {
        columns: [
            {name: 'time', type: 'time'},
            {name: 'event_id', type: 'string'},
            {name: 'temperature', type: 'number'},
            {name: 'labels', type: 'other'}
        ],
        rows: [[1234, '1:2:3:4', 23, {'host.name': 'dragon'}]],
        meta: {complete: false, reason: 'SOURCE_FAILED', laggards: [{writer_id: 2}], limited: false}
    };
    const frame = frames(response, 'A')[0];
    expect(frame.fields[0].values[0]).toBe(1234);
    expect(frame.fields[2].labels).toEqual({'host.name': 'dragon'});
    expect(frame.meta?.notices?.[0]).toEqual({severity: 'warning', text: 'SOURCE_FAILED; laggards: [{"writer_id":2}]'});
    response.meta = {...response.meta, complete: null, limited: true};
    expect(frames(response, 'A')[0].meta?.notices?.[0].severity).toBe('info');
    response.meta = {...response.meta, complete: true, limited: false};
    expect(frames(response, 'A')[0].meta?.notices).toEqual([]);
});
