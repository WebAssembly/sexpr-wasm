/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* polyfill from SM to D8 */
if (typeof arguments == 'undefined') {
  arguments = scriptArgs;
}

if (typeof readbuffer == 'undefined') {
  readbuffer = function(path) { return read(path, 'binary'); };
}

if (arguments.length != 1) {
  print('usage: <exe> spec.js -- <filename.json>');
  quit(0);
}

var passed = 0;
var failed = 0;

var quiet = false;

run(arguments[0]);

function run(inPath) {
  var lastSlash = inPath.lastIndexOf('/');
  var inDir = lastSlash == -1 ? '.' : inPath.slice(0, lastSlash);
  var data = read(inPath);
  var jsonData = JSON.parse(data);

  for (var i = 0; i < jsonData.modules.length; ++i) {
    var module = jsonData.modules[i];
    var moduleFile = readbuffer(inDir + '/' + module.filename);
    var m = createModule(moduleFile);
    for (var j = 0; j < module.commands.length; ++j) {
      var command = module.commands[j];
      switch (command.type) {
        case 'invoke':
          invoke(m, command.name);
          break;

        case 'assert_return':
          assertReturn(m, command.name, command.file, command.line);
          break;

        case 'assert_return_nan':
          assertReturn(m, command.name, command.file, command.line);
          break;

        case 'assert_trap':
          assertTrap(m, command.name, command.file, command.line);
          break;
      }
    }
  }
  end();
}

function createModule(a) {
  var memory = null;
  var u8a = new Uint8Array(a);
  var ffi = {spectest: {print: print}};
  var module = Wasm.instantiateModule(u8a, ffi);
  memory = module.memory;
  return module;
}

function assertReturn(m, name, file, line) {
  try {
    var result = m.exports[name]();
  } catch(e) {
    print(file + ":" + line + ": " + name + " unexpectedly threw: " + e);
  }

  if (result == 1) {
    passed++;
  } else {
    print(file + ":" + line + ": " + name + " failed.");
    failed++;
  }
}

function assertTrap(m, name, file, line) {
  var threw = false;
  try {
    m.exports[name]();
  } catch (e) {
    threw = true;
  }

  if (threw) {
    passed++;
  } else {
    print(file + ":" + line + ": " + name + " failed, didn't throw");
    failed++;
  }
}

function invoke(m, name) {
  try {
    var invokeResult = m.exports[name]();
  } catch(e) {
    print(file + ":" + line + ": " + name + " unexpectedly threw: " + e);
  }

  if (!quiet)
    print(name + " = " + invokeResult);
}

function end() {
  if ((failed > 0) || !quiet)
    print(passed + "/" + (passed + failed) + " tests passed.");
}
