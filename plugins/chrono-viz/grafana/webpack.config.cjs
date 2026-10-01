const path = require('path');
module.exports = {
  entry: './module.ts',
  output: { path: path.resolve(__dirname, 'dist'), filename: 'module.js', libraryTarget: 'amd' },
  resolve: { extensions: ['.ts', '.tsx', '.js'] },
  module: { rules: [{ test: /\.tsx?$/, loader: 'ts-loader', exclude: /node_modules/ }] },
  externals: ['@grafana/data', '@grafana/runtime', '@grafana/ui', 'react', 'react-dom', 'rxjs'],
};
