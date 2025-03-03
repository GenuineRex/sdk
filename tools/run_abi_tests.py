# Copyright (c) 2019, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

# Runs tests with old ABI versions and check if the results differ from the
# current results.

import json
import os
import subprocess
import sys
import time
import utils


scriptDir = os.path.dirname(os.path.realpath(__file__))
outDir = os.path.join(scriptDir, '..', 'out', 'ReleaseX64')


# Info about a running test.
class Test:
  def __init__(self, cmd, resultFile, logFile, version):
    self.cmd = cmd  # The test command.
    self.resultFile = resultFile  # The expected location of the result file.
    self.logFile = logFile  # The expected location of the log file.
    self.version = version  # The ABI version, or None.


# Recursively make directories for the path.
def makeDirs(path):
  try:
    os.makedirs(path)
  except OSError:
    pass


# Build a Test object for the given version (or None).
def buildTest(version):
  testDir = os.path.join(
    outDir, 'test%s' % ('' if version is None else str(version)))
  logDir = os.path.join(testDir, 'logs')
  makeDirs(logDir)

  vm_options = ['--enable-interpreter']
  if version is not None:
    vm_options += ['--use-abi-version=%d' % version]
  cmd = [
      'python',
      os.path.join(scriptDir, 'test.py'),
      '--compiler=dartkb',
      '--mode=release',
      '--write-results',
      '--write-logs',
      '--output_directory=%s' % logDir,
      '--vm-options=%s' % ' '.join(vm_options),
      'lib_2',
  ]

  resultFile = os.path.join(logDir, 'results.json')
  logFile = os.path.join(logDir, 'logs.json')
  return Test(cmd, resultFile, logFile, version)


# Build tests for every supported version, and return a list of Test objects.
def buildAllTests():
  abi_version = int(utils.GetAbiVersion())
  oldest_abi_version = int(utils.GetOldestSupportedAbiVersion())
  tests = [buildTest(None)]
  for version in xrange(oldest_abi_version, abi_version + 1):
    tests.append(buildTest(version))
  return tests


# Run all tests, one by one, and wait for them all to complete.
def runAllTests(tests):
  for test in tests:
    print('\n\n\n=== Running tests %s ===' % (
        ('for ABI version %d' % test.version) if test.version is not None else
        ('without an ABI version')))
    print(subprocess.list2cmdline(test.cmd) + '\n\n')
    proc = subprocess.Popen(test.cmd)
    while proc.returncode is None:
      time.sleep(1)
      proc.communicate()
      proc.poll()


# Read a test result file or log file and convert JSON lines to a dictionary of
# JSON records indexed by name. Assumes result and log files both use name key.
def readTestFile(fileName):
  with open(fileName, 'r') as f:
    return { r['name']: r for r in [json.loads(line) for line in f]}


# Read the test result or log files for every version and return a dict like:
# {name: {version: resultJson, ...}, ...}
def readAllTestFiles(tests, nameGetter):
  allRecords = {}
  for test in tests:
    records = readTestFile(nameGetter(test))
    for name, result in records.items():
      if name not in allRecords:
        allRecords[name] = {}
      allRecords[name][test.version] = result
  return allRecords


# Pick any element of the dictionary, favoring the None key if it exists.
def pickOne(d):
  if None in d:
    return d[None]
  for v in d.values():
    return v
  return None


# Diff the results of a test for each version and construct a new test result
# that reports whether the test results match for each version.
def diffResults(results):
  outResult = pickOne(results)
  exp = results[None]['result'] if None in results else None
  outResult['expected'] = exp
  outResult['result'] = exp
  outResult['matches'] = True
  diffs = []
  for version, result in results.items():
    if version is not None:
      act = result['result']
      if exp != act:
        diffs.append(version)
        outResult['result'] = act  # May be overwritten by other versions.
        outResult['matches'] = False
  return outResult, diffs


# Create a log entry for a test that has diffs. Concatenate all the log records
# and include which tests failed.
def makeLog(diffs, results, logRecords):
  result = pickOne(results)
  logs = ["%s: %s" % (str(v), l['log']) for v, l in logRecords.items()]
  return {
    'name': result['name'],
    'configuration': result['configuration'],
    'result': result['result'],
    'log': '\n\n\n'.join([repr(diffs)] + logs),
  }


# Diff the results of all the tests and create the merged result and log files.
def diffAllResults(tests):
  allResults = readAllTestFiles(tests, lambda test: test.resultFile)
  allLogs = readAllTestFiles(tests, lambda test: test.logFile)
  logDir = os.path.join(outDir, 'logs')
  makeDirs(logDir)
  resultFileName = os.path.join(logDir, 'results.json')
  logFileName = os.path.join(logDir, 'logs.json')
  with open(resultFileName, 'w') as resultFile:
    with open(logFileName, 'w') as logFile:
      for name, results in allResults.items():
        outResult, diffs = diffResults(results)
        resultFile.write(json.dumps(outResult) + '\n')
        if diffs:
          logRecords = allLogs[name] if name in allLogs else []
          logFile.write(json.dumps(makeLog(diffs, results, logRecords)) + '\n')


def main():
  tests = buildAllTests()
  runAllTests(tests)
  diffAllResults(tests)
  return 0


if __name__ == '__main__':
  sys.exit(main())
