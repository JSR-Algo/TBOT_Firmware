#!/usr/bin/env node

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

const CHECK = 'tvideoTeachingWordParity';

function report(value, exitCode) {
  process.stdout.write(`${JSON.stringify(value)}\n`);
  process.exitCode = exitCode;
}

function argumentValue(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

const managerRootArg = argumentValue('--manager-root');
const fixtureArg = argumentValue('--fixture');
if (!managerRootArg || !fixtureArg) {
  report({
    check: CHECK,
    status: 'ERROR',
    reason: 'required arguments: --manager-root <path> --fixture <path>',
  }, 2);
} else {
  try {
    const fixture = JSON.parse(fs.readFileSync(path.resolve(fixtureArg), 'utf8'));
    assert.equal(fixture.schemaVersion, 1, 'unsupported fixture schemaVersion');
    assert.equal(fixture.contract, CHECK, 'fixture contract name mismatch');
    assert.equal(typeof fixture.managerFile, 'string', 'fixture managerFile is required');
    assert.ok(Array.isArray(fixture.cases) && fixture.cases.length > 0, 'fixture cases are required');

    const source = fs.readFileSync(
      path.join(path.resolve(managerRootArg), fixture.managerFile),
      'utf8',
    );
    const match = /primaryWord\(\)\s*\{\s*return\s+([\s\S]*?);\s*\},/.exec(source);
    assert.ok(match, 'RobotLessonPreview primaryWord computed expression is missing');
    const primaryWord = Function(`return function primaryWord() { return (${match[1]}); }`)();

    for (const [caseIndex, testCase] of fixture.cases.entries()) {
      const actual = primaryWord.call({
        scene: {},
        body: testCase.body,
        currentStep: testCase.currentStep,
      });
      if (actual !== testCase.expected) {
        report({
          check: CHECK,
          status: 'FAIL',
          caseIndex,
          actual,
          expected: testCase.expected,
        }, 1);
        break;
      }
      if (caseIndex === fixture.cases.length - 1) {
        report({ check: CHECK, status: 'PASS', cases: fixture.cases.length }, 0);
      }
    }
  } catch (error) {
    report({
      check: CHECK,
      status: 'ERROR',
      reason: error instanceof Error ? error.message : String(error),
    }, 2);
  }
}
