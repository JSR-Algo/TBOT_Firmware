#!/usr/bin/env node

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';

const CHECK = 'staticFallbackImmediateRevealParity';

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
    const managerRoot = path.resolve(managerRootArg);
    const fixturePath = path.resolve(fixtureArg);
    const fixture = JSON.parse(fs.readFileSync(fixturePath, 'utf8'));

    assert.equal(fixture.schemaVersion, 1, 'unsupported fixture schemaVersion');
    assert.equal(fixture.contract, CHECK, 'fixture contract name mismatch');
    assert.equal(typeof fixture.managerFiles?.layouts, 'string', 'fixture missing manager layouts path');
    assert.equal(typeof fixture.managerFiles?.preview, 'string', 'fixture missing manager preview path');
    assert.ok(Array.isArray(fixture.behaviorCases), 'fixture behaviorCases must be an array');
    assert.ok(Array.isArray(fixture.fallbackRequestedPhases), 'fixture fallbackRequestedPhases must be an array');

    const layoutsPath = path.join(managerRoot, fixture.managerFiles.layouts);
    const previewPath = path.join(managerRoot, fixture.managerFiles.preview);
    const require = createRequire(pathToFileURL(layoutsPath));
    const layouts = require(layoutsPath);
    const previewSource = fs.readFileSync(previewPath, 'utf8');

    assert.equal(
      typeof layouts.isTeachingContentVisible,
      'function',
      'manager must export isTeachingContentVisible',
    );
    assert.equal(
      typeof layouts.effectivePreviewPhaseName,
      'function',
      'manager must export effectivePreviewPhaseName',
    );

    for (const behavior of fixture.behaviorCases) {
      assert.equal(
        layouts.isTeachingContentVisible(
          behavior.phase,
          fixture.revealPhase,
          behavior.staticFallback,
        ),
        behavior.contentVisible,
        `content visibility mismatch for phase=${behavior.phase} staticFallback=${behavior.staticFallback}`,
      );
    }

    for (const requestedPhase of fixture.fallbackRequestedPhases) {
      assert.equal(
        layouts.effectivePreviewPhaseName(requestedPhase, true),
        fixture.fallbackPhase,
        `fallback phase mismatch for requestedPhase=${requestedPhase}`,
      );
    }

    const requirements = fixture.componentRequirements;
    for (const helper of [requirements.visibilityHelper, requirements.effectivePhaseHelper]) {
      assert.ok(previewSource.includes(helper), `preview component missing ${helper}`);
    }
    assert.ok(
      previewSource.includes(requirements.fallbackPauseExpression),
      'preview fallback replay must remain paused on the arrived frame',
    );

    report({
      check: CHECK,
      status: 'PASS',
      fallbackPhase: fixture.fallbackPhase,
      revealPhase: fixture.revealPhase,
      behaviorCases: fixture.behaviorCases.length,
    }, 0);
  } catch (error) {
    report({
      check: CHECK,
      status: 'FAIL',
      reason: error instanceof Error ? error.message : String(error),
    }, 1);
  }
}
