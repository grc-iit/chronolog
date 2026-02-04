/**
 * Grafana default webpack configuration for plugins
 * This is a simplified version - the full version is provided by @grafana/create-plugin
 */

const path = require('path');
const { DefinePlugin } = require('webpack');
const CopyWebpackPlugin = require('copy-webpack-plugin');
const ForkTsCheckerWebpackPlugin = require('fork-ts-checker-webpack-plugin');
const ESLintPlugin = require('eslint-webpack-plugin');
const ReplaceInFileWebpackPlugin = require('replace-in-file-webpack-plugin');

const SOURCE_DIR = path.resolve(process.cwd(), 'src');
const DIST_DIR = path.resolve(process.cwd(), 'dist');

const config = async (env: Record<string, string>) => {
  const isProduction = env.production === 'true';

  return {
    mode: isProduction ? 'production' : 'development',
    target: 'web',
    context: SOURCE_DIR,
    devtool: isProduction ? 'source-map' : 'eval-source-map',

    entry: {
      module: './module.ts',
    },

    output: {
      clean: true,
      path: DIST_DIR,
      filename: '[name].js',
      library: {
        type: 'amd',
      },
    },

    externals: [
      'lodash',
      'react',
      'react-dom',
      '@grafana/data',
      '@grafana/runtime',
      '@grafana/ui',
      function (context, request, callback) {
        const prefix = 'grafana/';
        if (request?.startsWith(prefix)) {
          return callback(undefined, request.slice(prefix.length));
        }
        callback();
      },
    ],

    resolve: {
      extensions: ['.ts', '.tsx', '.js', '.jsx', '.json'],
    },

    module: {
      rules: [
        {
          test: /\.[tj]sx?$/,
          exclude: /node_modules/,
          use: {
            loader: 'swc-loader',
            options: {
              jsc: {
                parser: {
                  syntax: 'typescript',
                  tsx: true,
                  decorators: false,
                  dynamicImport: true,
                },
                target: 'es2020',
                transform: {
                  react: {
                    runtime: 'automatic',
                  },
                },
              },
            },
          },
        },
        {
          test: /\.css$/,
          use: ['style-loader', 'css-loader'],
        },
        {
          test: /\.s[ac]ss$/,
          use: ['style-loader', 'css-loader', 'sass-loader'],
        },
        {
          test: /\.(png|jpe?g|gif|svg)$/,
          type: 'asset/resource',
          generator: {
            filename: 'static/img/[name][ext]',
          },
        },
        {
          test: /\.(woff|woff2|eot|ttf|otf)$/,
          type: 'asset/resource',
          generator: {
            filename: 'static/fonts/[name][ext]',
          },
        },
      ],
    },

    plugins: [
      new CopyWebpackPlugin({
        patterns: [
          { from: 'plugin.json', to: '.' },
          { from: 'img', to: 'img', noErrorOnMissing: true },
          { from: '../README.md', to: '.', noErrorOnMissing: true },
        ],
      }),
      new ForkTsCheckerWebpackPlugin({
        typescript: {
          configFile: path.resolve(process.cwd(), 'tsconfig.json'),
        },
      }),
      new DefinePlugin({
        'process.env.NODE_ENV': JSON.stringify(isProduction ? 'production' : 'development'),
      }),
    ].filter(Boolean),

    optimization: {
      minimize: isProduction,
    },
  };
};

module.exports = config;

