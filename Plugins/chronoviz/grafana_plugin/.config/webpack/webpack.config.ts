import grafanaConfig = require('./.grafana/webpack.config');

const config = async (env: Record<string, string>) => {
  return grafanaConfig(env);
};

export = config;

