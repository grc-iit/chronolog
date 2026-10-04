// clang-format off
import React from 'react';
import { DataSourcePluginOptionsEditorProps } from '@grafana/data';
import { InlineField, Input } from '@grafana/ui';
import { Options } from './types';
export function ConfigEditor({options, onOptionsChange}: DataSourcePluginOptionsEditorProps<Options>) {
  return <InlineField label="Backend URL" tooltip="Address reachable from the Grafana server">
    <Input value={options.url || ''} placeholder="http://chrono-viz:8087"
      onChange={e => onOptionsChange({...options, access: 'proxy', url: e.currentTarget.value})} />
  </InlineField>;
}
