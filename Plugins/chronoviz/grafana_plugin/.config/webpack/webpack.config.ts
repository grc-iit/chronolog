const { merge } = require('webpack-merge');
const grafanaConfig = require('./.grafana/webpack.config.ts');
const path = require('path');

const config = async (env: Record<string, string>) => {
  const baseConfig = await grafanaConfig(env);

  return merge(baseConfig, {
    // Add custom webpack config here if needed
    resolve: {
      alias: {
        // Add path aliases if needed
      },
    },
  });
};

module.exports = config;

