import { ChangeEvent } from 'react';
import { DataSourcePluginOptionsEditorProps } from '@grafana/data';
import { InlineField, Input, FieldSet, SecretInput, Alert } from '@grafana/ui';
import { ChronoLogDataSourceOptions, ChronoLogSecureJsonData } from './types';

type Props = DataSourcePluginOptionsEditorProps<ChronoLogDataSourceOptions, ChronoLogSecureJsonData>;

/**
 * Configuration Editor component for ChronoLog data source
 *
 * Provides UI for configuring the connection to the ChronoLog backend service.
 */
export function ConfigEditor(props: Props) {
  const { onOptionsChange, options } = props;
  const { jsonData, secureJsonFields, secureJsonData } = options;

  const onBackendUrlChange = (event: ChangeEvent<HTMLInputElement>) => {
    onOptionsChange({
      ...options,
      jsonData: {
        ...jsonData,
        backendUrl: event.target.value,
      },
    });
  };

  const onDefaultChronicleChange = (event: ChangeEvent<HTMLInputElement>) => {
    onOptionsChange({
      ...options,
      jsonData: {
        ...jsonData,
        defaultChronicleName: event.target.value,
      },
    });
  };

  const onDefaultStoryChange = (event: ChangeEvent<HTMLInputElement>) => {
    onOptionsChange({
      ...options,
      jsonData: {
        ...jsonData,
        defaultStoryName: event.target.value,
      },
    });
  };

  const onClientConfFileChange = (event: ChangeEvent<HTMLInputElement>) => {
    onOptionsChange({
      ...options,
      jsonData: {
        ...jsonData,
        clientConfFile: event.target.value || undefined,
      },
    });
  };

  const onApiKeyChange = (event: ChangeEvent<HTMLInputElement>) => {
    onOptionsChange({
      ...options,
      secureJsonData: {
        ...secureJsonData,
        apiKey: event.target.value,
      },
    });
  };

  const onResetApiKey = () => {
    onOptionsChange({
      ...options,
      secureJsonFields: {
        ...secureJsonFields,
        apiKey: false,
      },
      secureJsonData: {
        ...secureJsonData,
        apiKey: '',
      },
    });
  };

  return (
    <>
      <Alert title="ChronoLog Data Source Configuration" severity="info">
        Configure the connection to the ChronoLog backend service. The backend service should be running and accessible
        from the Grafana server.
      </Alert>

      <FieldSet label="Connection Settings">
        <InlineField
          label="Backend URL"
          labelWidth={20}
          tooltip="URL of the ChronoLog backend service (e.g., http://localhost:8080)"
        >
          <Input
            value={jsonData.backendUrl || ''}
            onChange={onBackendUrlChange}
            placeholder="http://localhost:8080"
            width={50}
          />
        </InlineField>
        <InlineField
          label="Client Config File"
          labelWidth={20}
          tooltip="Optional path to ChronoLog client config JSON on the backend host. Leave empty to use default (install_dir/chronolog/conf/default_client_conf.json)"
        >
          <Input
            value={jsonData.clientConfFile || ''}
            onChange={onClientConfFileChange}
            placeholder="$CHRONOLOG_INSTALL_PATH/chronolog/conf/default_client_conf.json"
            width={50}
          />
        </InlineField>
      </FieldSet>

      <FieldSet label="Default Query Settings (Optional)">
        <InlineField
          label="Default Chronicle"
          labelWidth={20}
          tooltip="Default chronicle name to use for new queries (e.g., kfeng-EVO-X2)"
        >
          <Input
            value={jsonData.defaultChronicleName || ''}
            onChange={onDefaultChronicleChange}
            placeholder="kfeng-EVO-X2"
            width={50}
          />
        </InlineField>
        <InlineField
          label="Default Story"
          labelWidth={20}
          tooltip="Default story name to use for new queries (e.g., cpu_usage)"
        >
          <Input
            value={jsonData.defaultStoryName || ''}
            onChange={onDefaultStoryChange}
            placeholder="cpu_usage"
            width={50}
          />
        </InlineField>
      </FieldSet>

      <FieldSet label="Authentication (Optional)">
        <InlineField
          label="API Key"
          labelWidth={20}
          tooltip="Optional API key for backend authentication"
        >
          <SecretInput
            isConfigured={secureJsonFields?.apiKey ?? false}
            value={secureJsonData?.apiKey || ''}
            placeholder="Enter API key (optional)"
            width={50}
            onReset={onResetApiKey}
            onChange={onApiKeyChange}
          />
        </InlineField>
      </FieldSet>

      <FieldSet label="Setup Instructions">
        <div style={{ padding: '10px', backgroundColor: '#1e1e1e', borderRadius: '4px', fontSize: '13px' }}>
          <p style={{ marginBottom: '10px' }}>
            <strong>1. Start the ChronoLog backend service:</strong>
          </p>
          <pre style={{ backgroundColor: '#2d2d2d', padding: '8px', borderRadius: '4px', overflow: 'auto' }}>
            {`cd grafana_backend
pip install -r requirements.txt
python chronolog_service.py  # optional: --conf /path/to/client_conf.json`}
          </pre>

          <p style={{ marginTop: '15px', marginBottom: '10px' }}>
            <strong>2. Configure environment variables (optional):</strong>
          </p>
          <pre style={{ backgroundColor: '#2d2d2d', padding: '8px', borderRadius: '4px', overflow: 'auto' }}>
            {`export CHRONOLOG_PROTOCOL=ofi+sockets
export CHRONOLOG_PORTAL_IP=127.0.0.1
export CHRONOLOG_PORTAL_PORT=5555
export CHRONOLOG_PORTAL_PROVIDER_ID=55
export CHRONOLOG_QUERY_IP=127.0.0.1
export CHRONOLOG_QUERY_PORT=5557
export CHRONOLOG_QUERY_PROVIDER_ID=57
export CHRONOLOG_AUTO_CONNECT=true`}
          </pre>

          <p style={{ marginTop: '15px', marginBottom: '10px' }}>
            <strong>3. Test the connection:</strong>
          </p>
          <p>Click &quot;Save &amp; Test&quot; to verify connectivity to the backend service.</p>
        </div>
      </FieldSet>
    </>
  );
}

