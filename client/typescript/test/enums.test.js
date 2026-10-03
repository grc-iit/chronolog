const { test } = require('node:test');
const assert = require('node:assert/strict');
const native = require('../build/dev/chronolog_enum_test.node');
const { rejectionOf, FailedPrecondition } = require('../dist');

test('native status preserves unknown rejection 13', () => {
  const status = native.status();
  assert.equal(status.rejection, 'UNKNOWN_13');
  assert.equal(rejectionOf(status), 'UNKNOWN_13');
  const error = new FailedPrecondition(status.code, status.message, { code: 9, message: status.message }, status);
  assert.equal(rejectionOf(error), 'UNKNOWN_13');
});
