import {DataSourcePlugin} from '@grafana/data';
import {DataSource} from './datasource';
import {ConfigEditor} from './ConfigEditor';
import {Query, Options} from './types';
import {QueryEditor} from './QueryEditor';
export const plugin = new DataSourcePlugin<DataSource, Query, Options>(DataSource)
                              .setConfigEditor(ConfigEditor)
                              .setQueryEditor(QueryEditor);
