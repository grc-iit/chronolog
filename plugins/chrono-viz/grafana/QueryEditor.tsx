// clang-format off
import React, { useEffect, useState } from 'react';
import { QueryEditorProps, SelectableValue } from '@grafana/data';
import { InlineField, InlineFieldRow, Input, Select, Switch } from '@grafana/ui';
import { DataSource } from './datasource';
import { Query, Options } from './types';

export function QueryEditor({query, onChange, onRunQuery, datasource}: QueryEditorProps<DataSource, Query, Options>) {
  const [chronicles, setChronicles] = useState<string[]>([]);
  const [stories, setStories] = useState<string[]>([]);
  useEffect(() => {
    let active = true;
    datasource.choices().then(x => { if (active) { setChronicles(x); } }).catch(() => {});
    return () => { active = false; };
  }, [datasource]);
  useEffect(() => {
    let active = true;
    setStories([]);
    if (query.chronicle) {
      datasource.choices(query.chronicle).then(x => { if (active) { setStories(x); } }).catch(() => {});
    }
    return () => { active = false; };
  }, [datasource, query.chronicle]);
  const update = (value: Partial<Query>) => { onChange({...query, ...value}); onRunQuery(); };
  const options = (items: string[]): Array<SelectableValue<string>> => items.map(value => ({label: value, value}));
  return <InlineFieldRow>
    <InlineField label="Chronicle"><Select allowCustomValue options={options(chronicles)} value={query.chronicle}
      onChange={x => update({chronicle: x.value || '', story: ''})} /></InlineField>
    <InlineField label="Story"><Select allowCustomValue options={options(stories)} value={query.story}
      onChange={x => update({story: x.value || ''})} /></InlineField>
    <InlineField label="Fields"><Input value={(query.fields || []).join(',')} placeholder="temperature,pressure"
      onChange={e => onChange({...query, fields: e.currentTarget.value.split(',').map(x => x.trim()).filter(Boolean)})}
      onBlur={onRunQuery} /></InlineField>
    <InlineField label="Limit"><Input type="number" min={1} max={10000} value={query.limit || 10000}
      onChange={e => onChange({...query, limit: Number(e.currentTarget.value)})} onBlur={onRunQuery} /></InlineField>
    <InlineField label="Live"><Switch value={query.live || false} onChange={e => update({live: e.currentTarget.checked})} /></InlineField>
  </InlineFieldRow>;
}
