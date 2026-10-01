import {DataSourcePlugin} from '@grafana/data';
import {DataSource} from './datasource';
import {ConfigEditor} from './ConfigEditor';
import {QueryEditor} from './QueryEditor';
export const plugin = new DataSourcePlugin(DataSource).setConfigEditor(ConfigEditor).setQueryEditor(QueryEditor);
