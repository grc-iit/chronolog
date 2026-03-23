import { DataSourcePlugin } from '@grafana/data';
import { DataSource } from './datasource';
import { ConfigEditor } from './ConfigEditor';
import { QueryEditor } from './QueryEditor';
import { ChronoLogQuery, ChronoLogDataSourceOptions } from './types';

/**
 * ChronoLog Data Source Plugin for Grafana
 *
 * This plugin provides connectivity to ChronoLog distributed log store,
 * enabling visualization and analysis of log events stored in chronicles and stories.
 */
export const plugin = new DataSourcePlugin<DataSource, ChronoLogQuery, ChronoLogDataSourceOptions>(DataSource)
  .setConfigEditor(ConfigEditor)
  .setQueryEditor(QueryEditor);

