import { useState, useEffect, useCallback } from 'react';
import { QueryEditorProps, SelectableValue } from '@grafana/data';
import { InlineField, Input, Select, InlineFieldRow } from '@grafana/ui';
import { DataSource } from './datasource';
import { ChronoLogDataSourceOptions, ChronoLogQuery } from './types';

type Props = QueryEditorProps<DataSource, ChronoLogQuery, ChronoLogDataSourceOptions>;

/**
 * Query Editor component for ChronoLog data source
 *
 * Provides UI for selecting chronicle and story to query,
 * with autocomplete support for available options.
 */
export function QueryEditor({ query, onChange, onRunQuery, datasource }: Props) {
  const [chronicles, setChronicles] = useState<Array<SelectableValue<string>>>([]);
  const [stories, setStories] = useState<Array<SelectableValue<string>>>([]);
  const [chroniclesLoading, setChroniclesLoading] = useState(false);
  const [storiesLoading, setStoriesLoading] = useState(false);

  // Load chronicles on mount
  // Note: Backend /chronicles endpoint may not be implemented yet
  // This will gracefully return empty list if endpoint is unavailable
  useEffect(() => {
    const loadChronicles = async () => {
      setChroniclesLoading(true);
      try {
        const chronicleList = await datasource.getChronicles();
        setChronicles(
          chronicleList.map((c) => ({
            label: c,
            value: c,
          }))
        );
        // If no chronicles returned, allow manual entry
        if (chronicleList.length === 0) {
          console.debug('No chronicles returned from backend - endpoint may not be implemented yet');
        }
      } catch (error) {
        console.error('Failed to load chronicles:', error);
        setChronicles([]);
        // Allow manual entry even if endpoint fails
      } finally {
        setChroniclesLoading(false);
      }
    };

    loadChronicles();
  }, [datasource]);

  // Load stories when chronicle changes
  // Note: Backend /stories endpoint may not be implemented yet
  // This will gracefully return empty list if endpoint is unavailable
  useEffect(() => {
    const loadStories = async () => {
      if (!query.chronicleName) {
        setStories([]);
        return;
      }

      setStoriesLoading(true);
      try {
        const storyList = await datasource.getStories(query.chronicleName);
        setStories(
          storyList.map((s) => ({
            label: s,
            value: s,
          }))
        );
        // If no stories returned, allow manual entry
        if (storyList.length === 0) {
          console.debug(`No stories returned from backend for ${query.chronicleName} - endpoint may not be implemented yet`);
        }
      } catch (error) {
        console.error('Failed to load stories:', error);
        setStories([]);
        // Allow manual entry even if endpoint fails
      } finally {
        setStoriesLoading(false);
      }
    };

    loadStories();
  }, [datasource, query.chronicleName]);

  const onChronicleChange = useCallback(
    (value: SelectableValue<string>) => {
      onChange({
        ...query,
        chronicleName: value.value || '',
        // Clear story when chronicle changes
        storyName: '',
      });
    },
    [onChange, query]
  );

  const onStoryChange = useCallback(
    (value: SelectableValue<string>) => {
      onChange({
        ...query,
        storyName: value.value || '',
      });
      // Run query when story is selected
      onRunQuery();
    },
    [onChange, onRunQuery, query]
  );

  const onChronicleInputChange = useCallback(
    (event: React.FormEvent<HTMLInputElement>) => {
      const value = event.currentTarget.value;
      onChange({
        ...query,
        chronicleName: value,
      });
    },
    [onChange, query]
  );

  const onStoryInputChange = useCallback(
    (event: React.FormEvent<HTMLInputElement>) => {
      const value = event.currentTarget.value;
      onChange({
        ...query,
        storyName: value,
      });
    },
    [onChange, query]
  );

  const onKeyDown = useCallback(
    (event: React.KeyboardEvent) => {
      if (event.key === 'Enter') {
        onRunQuery();
      }
    },
    [onRunQuery]
  );

  // Get current selections
  const selectedChronicle = chronicles.find((c) => c.value === query.chronicleName) || {
    label: query.chronicleName || '',
    value: query.chronicleName || '',
  };

  const selectedStory = stories.find((s) => s.value === query.storyName) || {
    label: query.storyName || '',
    value: query.storyName || '',
  };

  return (
    <div className="gf-form-group">
      <InlineFieldRow>
        <InlineField
          label="Chronicle"
          labelWidth={14}
          tooltip="Name of the chronicle to query. Select from dropdown or enter manually."
          grow
        >
          {chronicles.length > 0 ? (
            <Select
              value={selectedChronicle}
              options={chronicles}
              onChange={onChronicleChange}
              isLoading={chroniclesLoading}
              allowCustomValue
              onCreateOption={(value) => {
                onChange({ ...query, chronicleName: value, storyName: '' });
              }}
              placeholder="Select or enter chronicle name"
              isClearable
              width={40}
            />
          ) : (
            <Input
              value={query.chronicleName || ''}
              onChange={onChronicleInputChange}
              onKeyDown={onKeyDown}
              placeholder="Enter chronicle name"
              width={40}
            />
          )}
        </InlineField>
      </InlineFieldRow>

      <InlineFieldRow>
        <InlineField
          label="Story"
          labelWidth={14}
          tooltip="Name of the story within the chronicle. Select from dropdown or enter manually."
          grow
        >
          {stories.length > 0 ? (
            <Select
              value={selectedStory}
              options={stories}
              onChange={onStoryChange}
              isLoading={storiesLoading}
              allowCustomValue
              onCreateOption={(value) => {
                onChange({ ...query, storyName: value });
                onRunQuery();
              }}
              placeholder="Select or enter story name"
              isClearable
              width={40}
              disabled={!query.chronicleName}
            />
          ) : (
            <Input
              value={query.storyName || ''}
              onChange={onStoryInputChange}
              onKeyDown={onKeyDown}
              placeholder="Enter story name"
              width={40}
              disabled={!query.chronicleName}
            />
          )}
        </InlineField>
      </InlineFieldRow>

      {query.chronicleName && query.storyName && (
        <div style={{ marginTop: '8px', fontSize: '12px', color: '#8e8e8e' }}>
          Querying: <strong>{query.chronicleName}</strong> / <strong>{query.storyName}</strong>
        </div>
      )}
    </div>
  );
}

