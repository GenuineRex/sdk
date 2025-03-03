// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Generate a snapshot file after loading all the scripts specified on the
// command line.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cstdarg>
#include <memory>

#include "bin/builtin.h"
#include "bin/console.h"
#include "bin/dartutils.h"
#include "bin/eventhandler.h"
#include "bin/file.h"
#include "bin/loader.h"
#include "bin/options.h"
#include "bin/platform.h"
#include "bin/snapshot_utils.h"
#include "bin/thread.h"
#include "bin/utils.h"
#include "bin/vmservice_impl.h"
#include "platform/syslog.h"

#include "include/dart_api.h"
#include "include/dart_tools_api.h"

#include "platform/globals.h"
#include "platform/growable_array.h"
#include "platform/hashmap.h"

namespace dart {
namespace bin {

// Exit code indicating an API error.
static const int kApiErrorExitCode = 253;
// Exit code indicating a compilation error.
static const int kCompilationErrorExitCode = 254;
// Exit code indicating an unhandled error that is not a compilation error.
static const int kErrorExitCode = 255;

#define CHECK_RESULT(result)                                                   \
  if (Dart_IsError(result)) {                                                  \
    intptr_t exit_code = 0;                                                    \
    Syslog::PrintErr("Error: %s\n", Dart_GetError(result));                    \
    if (Dart_IsCompilationError(result)) {                                     \
      exit_code = kCompilationErrorExitCode;                                   \
    } else if (Dart_IsApiError(result)) {                                      \
      exit_code = kApiErrorExitCode;                                           \
    } else {                                                                   \
      exit_code = kErrorExitCode;                                              \
    }                                                                          \
    Dart_ExitScope();                                                          \
    Dart_ShutdownIsolate();                                                    \
    exit(exit_code);                                                           \
  }

// The environment provided through the command line using -D options.
static dart::SimpleHashMap* environment = NULL;

static bool ProcessEnvironmentOption(const char* arg,
                                     CommandLineOptions* vm_options) {
  return OptionProcessor::ProcessEnvironmentOption(arg, vm_options,
                                                   &environment);
}

// The core snapshot to use when creating isolates. Normally NULL, but loaded
// from a file when creating AppJIT snapshots.
const uint8_t* isolate_snapshot_data = NULL;
const uint8_t* isolate_snapshot_instructions = NULL;

// Global state that indicates whether a snapshot is to be created and
// if so which file to write the snapshot into. The ordering of this list must
// match kSnapshotKindNames below.
enum SnapshotKind {
  kCore,
  kCoreJIT,
  kApp,
  kAppJIT,
  kAppAOTBlobs,
  kAppAOTAssembly,
  kAppAOTElf,
  kVMAOTAssembly,
};
static SnapshotKind snapshot_kind = kCore;

// The ordering of this list must match the SnapshotKind enum above.
static const char* kSnapshotKindNames[] = {
    // clang-format off
    "core",
    "core-jit",
    "app",
    "app-jit",
    "app-aot-blobs",
    "app-aot-assembly",
    "app-aot-elf",
    "vm-aot-assembly",
    NULL,
    // clang-format on
};

#define STRING_OPTIONS_LIST(V)                                                 \
  V(load_vm_snapshot_data, load_vm_snapshot_data_filename)                     \
  V(load_vm_snapshot_instructions, load_vm_snapshot_instructions_filename)     \
  V(load_isolate_snapshot_data, load_isolate_snapshot_data_filename)           \
  V(load_isolate_snapshot_instructions,                                        \
    load_isolate_snapshot_instructions_filename)                               \
  V(vm_snapshot_data, vm_snapshot_data_filename)                               \
  V(vm_snapshot_instructions, vm_snapshot_instructions_filename)               \
  V(isolate_snapshot_data, isolate_snapshot_data_filename)                     \
  V(isolate_snapshot_instructions, isolate_snapshot_instructions_filename)     \
  V(shared_data, shared_data_filename)                                         \
  V(shared_instructions, shared_instructions_filename)                         \
  V(shared_blobs, shared_blobs_filename)                                       \
  V(reused_instructions, reused_instructions_filename)                         \
  V(blobs_container_filename, blobs_container_filename)                        \
  V(assembly, assembly_filename)                                               \
  V(elf, elf_filename)                                                         \
  V(load_compilation_trace, load_compilation_trace_filename)                   \
  V(load_type_feedback, load_type_feedback_filename)                           \
  V(save_obfuscation_map, obfuscation_map_filename)

#define BOOL_OPTIONS_LIST(V)                                                   \
  V(compile_all, compile_all)                                                  \
  V(help, help)                                                                \
  V(obfuscate, obfuscate)                                                      \
  V(read_all_bytecode, read_all_bytecode)                                      \
  V(strip, strip)                                                              \
  V(verbose, verbose)                                                          \
  V(version, version)

#define STRING_OPTION_DEFINITION(flag, variable)                               \
  static const char* variable = NULL;                                          \
  DEFINE_STRING_OPTION(flag, variable)
STRING_OPTIONS_LIST(STRING_OPTION_DEFINITION)
#undef STRING_OPTION_DEFINITION

#define BOOL_OPTION_DEFINITION(flag, variable)                                 \
  static bool variable = false;                                                \
  DEFINE_BOOL_OPTION(flag, variable)
BOOL_OPTIONS_LIST(BOOL_OPTION_DEFINITION)
#undef BOOL_OPTION_DEFINITION

DEFINE_ENUM_OPTION(snapshot_kind, SnapshotKind, snapshot_kind);
DEFINE_CB_OPTION(ProcessEnvironmentOption);

static bool IsSnapshottingForPrecompilation() {
  return (snapshot_kind == kAppAOTBlobs) ||
         (snapshot_kind == kAppAOTAssembly) || (snapshot_kind == kAppAOTElf) ||
         (snapshot_kind == kVMAOTAssembly);
}

// clang-format off
static void PrintUsage() {
  Syslog::PrintErr(
"Usage: gen_snapshot [<vm-flags>] [<options>] <dart-kernel-file>             \n"
"                                                                            \n"
"Common options:                                                             \n"
"--help                                                                      \n"
"  Display this message (add --verbose for information about all VM options).\n"
"--version                                                                   \n"
"  Print the VM version.                                                     \n"
"                                                                            \n"
"To create a core snapshot:                                                  \n"
"--snapshot_kind=core                                                        \n"
"--vm_snapshot_data=<output-file>                                            \n"
"--isolate_snapshot_data=<output-file>                                       \n"
"<dart-kernel-file>                                                          \n"
"                                                                            \n"
"To create an AOT application snapshot as blobs suitable for loading with    \n"
"mmap:                                                                       \n"
"--snapshot_kind=app-aot-blobs                                               \n"
"--vm_snapshot_data=<output-file>                                            \n"
"--vm_snapshot_instructions=<output-file>                                    \n"
"--isolate_snapshot_data=<output-file>                                       \n"
"--isolate_snapshot_instructions=<output-file>                               \n"
"[--obfuscate]                                                               \n"
"[--save-obfuscation-map=<map-filename>]                                     \n"
"<dart-kernel-file>                                                          \n"
"                                                                            \n"
"To create an AOT application snapshot as assembly suitable for compilation  \n"
"as a static or dynamic library:                                             \n"
"--snapshot_kind=app-aot-assembly                                            \n"
"--assembly=<output-file>                                                    \n"
"[--obfuscate]                                                               \n"
"[--save-obfuscation-map=<map-filename>]                                     \n"
"<dart-kernel-file>                                                          \n"
"                                                                            \n"
"To create an AOT application snapshot as an ELF shared library:             \n"
"--snapshot_kind=app-aot-elf                                                 \n"
"--elf=<output-file>                                                         \n"
"[--strip]                                                                   \n"
"[--obfuscate]                                                               \n"
"[--save-obfuscation-map=<map-filename>]                                     \n"
"<dart-kernel-file>                                                          \n"
"                                                                            \n"
"AOT snapshots can be obfuscated: that is all identifiers will be renamed    \n"
"during compilation. This mode is enabled with --obfuscate flag. Mapping     \n"
"between original and obfuscated names can be serialized as a JSON array     \n"
"using --save-obfuscation-map=<filename> option. See dartbug.com/30524       \n"
"for implementation details and limitations of the obfuscation pass.         \n"
"                                                                            \n"
"\n");
  if (verbose) {
    Syslog::PrintErr(
"The following options are only used for VM development and may\n"
"be changed in any future version:\n");
    const char* print_flags = "--print_flags";
    char* error = Dart_SetVMFlags(1, &print_flags);
    ASSERT(error == NULL);
  }
}
// clang-format on

// Parse out the command line arguments. Returns -1 if the arguments
// are incorrect, 0 otherwise.
static int ParseArguments(int argc,
                          char** argv,
                          CommandLineOptions* vm_options,
                          CommandLineOptions* inputs) {
  const char* kPrefix = "-";
  const intptr_t kPrefixLen = strlen(kPrefix);

  // Skip the binary name.
  int i = 1;

  // Parse out the vm options.
  while ((i < argc) &&
         OptionProcessor::IsValidFlag(argv[i], kPrefix, kPrefixLen)) {
    if (OptionProcessor::TryProcess(argv[i], vm_options)) {
      i += 1;
      continue;
    }
    vm_options->AddArgument(argv[i]);
    i += 1;
  }

  // Parse out the kernel inputs.
  while (i < argc) {
    inputs->AddArgument(argv[i]);
    i++;
  }

  if (help) {
    PrintUsage();
    Platform::Exit(0);
  } else if (version) {
    Syslog::PrintErr("Dart VM version: %s\n", Dart_VersionString());
    Platform::Exit(0);
  }

  // Verify consistency of arguments.
  if (inputs->count() < 1) {
    Syslog::PrintErr("At least one input is required\n");
    return -1;
  }

  switch (snapshot_kind) {
    case kCore: {
      if ((vm_snapshot_data_filename == NULL) ||
          (isolate_snapshot_data_filename == NULL)) {
        Syslog::PrintErr(
            "Building a core snapshot requires specifying output files for "
            "--vm_snapshot_data and --isolate_snapshot_data.\n\n");
        return -1;
      }
      break;
    }
    case kCoreJIT: {
      if ((vm_snapshot_data_filename == NULL) ||
          (vm_snapshot_instructions_filename == NULL) ||
          (isolate_snapshot_data_filename == NULL) ||
          (isolate_snapshot_instructions_filename == NULL)) {
        Syslog::PrintErr(
            "Building a core JIT snapshot requires specifying output "
            "files for --vm_snapshot_data, --vm_snapshot_instructions, "
            "--isolate_snapshot_data and --isolate_snapshot_instructions.\n\n");
        return -1;
      }
      break;
    }
    case kApp:
    case kAppJIT: {
      if ((load_vm_snapshot_data_filename == NULL) ||
          (isolate_snapshot_data_filename == NULL) ||
          ((isolate_snapshot_instructions_filename == NULL) &&
           (reused_instructions_filename == NULL))) {
        Syslog::PrintErr(
            "Building an app JIT snapshot requires specifying input files for "
            "--load_vm_snapshot_data and --load_vm_snapshot_instructions, an "
            " output file for --isolate_snapshot_data, and either an output "
            "file for --isolate_snapshot_instructions or an input file for "
            "--reused_instructions.\n\n");
        return -1;
      }
      break;
    }
    case kAppAOTBlobs: {
      if ((blobs_container_filename == NULL) &&
          ((vm_snapshot_data_filename == NULL) ||
           (vm_snapshot_instructions_filename == NULL) ||
           (isolate_snapshot_data_filename == NULL) ||
           (isolate_snapshot_instructions_filename == NULL))) {
        Syslog::PrintErr(
            "Building an AOT snapshot as blobs requires specifying output "
            "file for --blobs_container_filename or "
            "files for --vm_snapshot_data, --vm_snapshot_instructions, "
            "--isolate_snapshot_data and --isolate_snapshot_instructions.\n\n");
        return -1;
      }
      if ((blobs_container_filename != NULL) &&
          ((vm_snapshot_data_filename != NULL) ||
           (vm_snapshot_instructions_filename != NULL) ||
           (isolate_snapshot_data_filename != NULL) ||
           (isolate_snapshot_instructions_filename != NULL))) {
        Syslog::PrintErr(
            "Building an AOT snapshot as blobs requires specifying output "
            "file for --blobs_container_filename or "
            "files for --vm_snapshot_data, --vm_snapshot_instructions, "
            "--isolate_snapshot_data and --isolate_snapshot_instructions"
            " not both.\n\n");
        return -1;
      }
      break;
    }
    case kAppAOTElf: {
      if (elf_filename == NULL) {
        Syslog::PrintErr(
            "Building an AOT snapshot as assembly requires specifying "
            "an output file for --elf.\n\n");
        return -1;
      }
      break;
    }
    case kAppAOTAssembly:
    case kVMAOTAssembly: {
      if (assembly_filename == NULL) {
        Syslog::PrintErr(
            "Building an AOT snapshot as assembly requires specifying "
            "an output file for --assembly.\n\n");
        return -1;
      }
      break;
    }
  }

  if (!obfuscate && obfuscation_map_filename != NULL) {
    Syslog::PrintErr(
        "--obfuscation_map=<...> should only be specified when obfuscation is "
        "enabled by --obfuscate flag.\n\n");
    return -1;
  }

  if (obfuscate && !IsSnapshottingForPrecompilation()) {
    Syslog::PrintErr(
        "Obfuscation can only be enabled when building AOT snapshot.\n\n");
    return -1;
  }

  return 0;
}

static File* OpenFile(const char* filename) {
  File* file = File::Open(NULL, filename, File::kWriteTruncate);
  if (file == NULL) {
    Syslog::PrintErr("Error: Unable to write file: %s\n\n", filename);
    Dart_ExitScope();
    Dart_ShutdownIsolate();
    exit(kErrorExitCode);
  }
  return file;
}

static void WriteFile(const char* filename,
                      const uint8_t* buffer,
                      const intptr_t size) {
  File* file = OpenFile(filename);
  RefCntReleaseScope<File> rs(file);
  if (!file->WriteFully(buffer, size)) {
    Syslog::PrintErr("Error: Unable to write file: %s\n\n", filename);
    Dart_ExitScope();
    Dart_ShutdownIsolate();
    exit(kErrorExitCode);
  }
}

static void ReadFile(const char* filename, uint8_t** buffer, intptr_t* size) {
  File* file = File::Open(NULL, filename, File::kRead);
  if (file == NULL) {
    Syslog::PrintErr("Unable to open file %s\n", filename);
    Dart_ExitScope();
    Dart_ShutdownIsolate();
    exit(kErrorExitCode);
  }
  RefCntReleaseScope<File> rs(file);
  *size = file->Length();
  *buffer = reinterpret_cast<uint8_t*>(malloc(*size));
  if (!file->ReadFully(*buffer, *size)) {
    Syslog::PrintErr("Unable to read file %s\n", filename);
    Dart_ExitScope();
    Dart_ShutdownIsolate();
    exit(kErrorExitCode);
  }
}

static void MaybeLoadExtraInputs(const CommandLineOptions& inputs) {
  for (intptr_t i = 1; i < inputs.count(); i++) {
    uint8_t* buffer = NULL;
    intptr_t size = 0;
    ReadFile(inputs.GetArgument(i), &buffer, &size);
    Dart_Handle result = Dart_LoadLibraryFromKernel(buffer, size);
    CHECK_RESULT(result);
  }
}

static void MaybeLoadCode() {
  if (read_all_bytecode &&
      ((snapshot_kind == kCore) || (snapshot_kind == kCoreJIT) ||
       (snapshot_kind == kApp) || (snapshot_kind == kAppJIT))) {
    Dart_Handle result = Dart_ReadAllBytecode();
    CHECK_RESULT(result);
  }

  if (compile_all &&
      ((snapshot_kind == kCoreJIT) || (snapshot_kind == kAppJIT))) {
    Dart_Handle result = Dart_CompileAll();
    CHECK_RESULT(result);
  }

  if ((load_compilation_trace_filename != NULL) &&
      ((snapshot_kind == kCoreJIT) || (snapshot_kind == kAppJIT))) {
    // Finalize all classes. This ensures that there are no non-finalized
    // classes in the gaps between cid ranges. Such classes prevent merging of
    // cid ranges.
    Dart_Handle result = Dart_FinalizeAllClasses();
    CHECK_RESULT(result);
    // Sort classes to have better cid ranges.
    result = Dart_SortClasses();
    CHECK_RESULT(result);
    uint8_t* buffer = NULL;
    intptr_t size = 0;
    ReadFile(load_compilation_trace_filename, &buffer, &size);
    result = Dart_LoadCompilationTrace(buffer, size);
    free(buffer);
    CHECK_RESULT(result);
  }

  if ((load_type_feedback_filename != NULL) &&
      ((snapshot_kind == kCoreJIT) || (snapshot_kind == kAppJIT))) {
    uint8_t* buffer = NULL;
    intptr_t size = 0;
    ReadFile(load_type_feedback_filename, &buffer, &size);
    Dart_Handle result = Dart_LoadTypeFeedback(buffer, size);
    free(buffer);
    CHECK_RESULT(result);
  }
}

static void CreateAndWriteCoreSnapshot() {
  ASSERT(snapshot_kind == kCore);
  ASSERT(vm_snapshot_data_filename != NULL);
  ASSERT(isolate_snapshot_data_filename != NULL);

  Dart_Handle result;
  uint8_t* vm_snapshot_data_buffer = NULL;
  intptr_t vm_snapshot_data_size = 0;
  uint8_t* isolate_snapshot_data_buffer = NULL;
  intptr_t isolate_snapshot_data_size = 0;

  // First create a snapshot.
  result = Dart_CreateSnapshot(&vm_snapshot_data_buffer, &vm_snapshot_data_size,
                               &isolate_snapshot_data_buffer,
                               &isolate_snapshot_data_size);
  CHECK_RESULT(result);

  // Now write the vm isolate and isolate snapshots out to the
  // specified file and exit.
  WriteFile(vm_snapshot_data_filename, vm_snapshot_data_buffer,
            vm_snapshot_data_size);
  if (vm_snapshot_instructions_filename != NULL) {
    // Create empty file for the convenience of build systems. Makes things
    // polymorphic with generating core-jit snapshots.
    WriteFile(vm_snapshot_instructions_filename, NULL, 0);
  }
  WriteFile(isolate_snapshot_data_filename, isolate_snapshot_data_buffer,
            isolate_snapshot_data_size);
  if (isolate_snapshot_instructions_filename != NULL) {
    // Create empty file for the convenience of build systems. Makes things
    // polymorphic with generating core-jit snapshots.
    WriteFile(isolate_snapshot_instructions_filename, NULL, 0);
  }
}

static std::unique_ptr<MappedMemory> MapFile(const char* filename,
                                             File::MapType type,
                                             const uint8_t** buffer) {
  File* file = File::Open(NULL, filename, File::kRead);
  if (file == NULL) {
    Syslog::PrintErr("Failed to open: %s\n", filename);
    exit(kErrorExitCode);
  }
  RefCntReleaseScope<File> rs(file);
  intptr_t length = file->Length();
  if (length == 0) {
    // Can't map an empty file.
    *buffer = NULL;
    return NULL;
  }
  MappedMemory* mapping = file->Map(type, 0, length);
  if (mapping == NULL) {
    Syslog::PrintErr("Failed to read: %s\n", filename);
    exit(kErrorExitCode);
  }
  *buffer = reinterpret_cast<const uint8_t*>(mapping->address());
  return std::unique_ptr<MappedMemory>(mapping);
}

static void CreateAndWriteCoreJITSnapshot() {
  ASSERT(snapshot_kind == kCoreJIT);
  ASSERT(vm_snapshot_data_filename != NULL);
  ASSERT(vm_snapshot_instructions_filename != NULL);
  ASSERT(isolate_snapshot_data_filename != NULL);
  ASSERT(isolate_snapshot_instructions_filename != NULL);

  Dart_Handle result;
  uint8_t* vm_snapshot_data_buffer = NULL;
  intptr_t vm_snapshot_data_size = 0;
  uint8_t* vm_snapshot_instructions_buffer = NULL;
  intptr_t vm_snapshot_instructions_size = 0;
  uint8_t* isolate_snapshot_data_buffer = NULL;
  intptr_t isolate_snapshot_data_size = 0;
  uint8_t* isolate_snapshot_instructions_buffer = NULL;
  intptr_t isolate_snapshot_instructions_size = 0;

  // First create a snapshot.
  result = Dart_CreateCoreJITSnapshotAsBlobs(
      &vm_snapshot_data_buffer, &vm_snapshot_data_size,
      &vm_snapshot_instructions_buffer, &vm_snapshot_instructions_size,
      &isolate_snapshot_data_buffer, &isolate_snapshot_data_size,
      &isolate_snapshot_instructions_buffer,
      &isolate_snapshot_instructions_size);
  CHECK_RESULT(result);

  // Now write the vm isolate and isolate snapshots out to the
  // specified file and exit.
  WriteFile(vm_snapshot_data_filename, vm_snapshot_data_buffer,
            vm_snapshot_data_size);
  WriteFile(vm_snapshot_instructions_filename, vm_snapshot_instructions_buffer,
            vm_snapshot_instructions_size);
  WriteFile(isolate_snapshot_data_filename, isolate_snapshot_data_buffer,
            isolate_snapshot_data_size);
  WriteFile(isolate_snapshot_instructions_filename,
            isolate_snapshot_instructions_buffer,
            isolate_snapshot_instructions_size);
}

static void CreateAndWriteAppSnapshot() {
  ASSERT(snapshot_kind == kApp);
  ASSERT(isolate_snapshot_data_filename != NULL);

  Dart_Handle result;
  uint8_t* isolate_snapshot_data_buffer = NULL;
  intptr_t isolate_snapshot_data_size = 0;

  result = Dart_CreateSnapshot(NULL, NULL, &isolate_snapshot_data_buffer,
                               &isolate_snapshot_data_size);
  CHECK_RESULT(result);

  WriteFile(isolate_snapshot_data_filename, isolate_snapshot_data_buffer,
            isolate_snapshot_data_size);
  if (isolate_snapshot_instructions_filename != NULL) {
    // Create empty file for the convenience of build systems. Makes things
    // polymorphic with generating core-jit snapshots.
    WriteFile(isolate_snapshot_instructions_filename, NULL, 0);
  }
}

static void CreateAndWriteAppJITSnapshot() {
  ASSERT(snapshot_kind == kAppJIT);
  ASSERT(isolate_snapshot_data_filename != NULL);
  ASSERT((isolate_snapshot_instructions_filename != NULL) ||
         (reused_instructions_filename != NULL));

  const uint8_t* reused_instructions = NULL;
  std::unique_ptr<MappedMemory> mapped_reused_instructions;
  if (reused_instructions_filename != NULL) {
    mapped_reused_instructions = MapFile(reused_instructions_filename,
                                         File::kReadOnly, &reused_instructions);
  }

  Dart_Handle result;
  uint8_t* isolate_snapshot_data_buffer = NULL;
  intptr_t isolate_snapshot_data_size = 0;
  uint8_t* isolate_snapshot_instructions_buffer = NULL;
  intptr_t isolate_snapshot_instructions_size = 0;

  result = Dart_CreateAppJITSnapshotAsBlobs(
      &isolate_snapshot_data_buffer, &isolate_snapshot_data_size,
      &isolate_snapshot_instructions_buffer,
      &isolate_snapshot_instructions_size, reused_instructions);
  CHECK_RESULT(result);

  WriteFile(isolate_snapshot_data_filename, isolate_snapshot_data_buffer,
            isolate_snapshot_data_size);
  if (reused_instructions_filename == NULL) {
    WriteFile(isolate_snapshot_instructions_filename,
              isolate_snapshot_instructions_buffer,
              isolate_snapshot_instructions_size);
  }
}

static void StreamingWriteCallback(void* callback_data,
                                   const uint8_t* buffer,
                                   intptr_t size) {
  File* file = reinterpret_cast<File*>(callback_data);
  if (!file->WriteFully(buffer, size)) {
    Syslog::PrintErr("Error: Unable to write snapshot file\n\n");
    Dart_ExitScope();
    Dart_ShutdownIsolate();
    exit(kErrorExitCode);
  }
}

static void CreateAndWritePrecompiledSnapshot() {
  ASSERT(IsSnapshottingForPrecompilation());
  Dart_Handle result;

  // Precompile with specified embedder entry points
  result = Dart_Precompile();
  CHECK_RESULT(result);

  // Create a precompiled snapshot.
  if (snapshot_kind == kAppAOTAssembly) {
    File* file = OpenFile(assembly_filename);
    RefCntReleaseScope<File> rs(file);
    result = Dart_CreateAppAOTSnapshotAsAssembly(StreamingWriteCallback, file);
    CHECK_RESULT(result);
  } else if (snapshot_kind == kAppAOTElf) {
    if (strip) {
      Syslog::PrintErr(
          "Warning: Generating ELF library without DWARF debugging"
          " information.\n");
    }
    File* file = OpenFile(elf_filename);
    RefCntReleaseScope<File> rs(file);
    result =
        Dart_CreateAppAOTSnapshotAsElf(StreamingWriteCallback, file, strip);
    CHECK_RESULT(result);
  } else if (snapshot_kind == kAppAOTBlobs) {
    const uint8_t* shared_data = NULL;
    const uint8_t* shared_instructions = NULL;
    std::unique_ptr<MappedMemory> mapped_shared_data;
    std::unique_ptr<MappedMemory> mapped_shared_instructions;
    if (shared_blobs_filename != NULL) {
      AppSnapshot* shared_blobs = NULL;
      Syslog::PrintErr("Shared blobs in gen_snapshot are for testing only.\n");
      shared_blobs = Snapshot::TryReadAppSnapshot(shared_blobs_filename);
      if (shared_blobs == NULL) {
        Syslog::PrintErr("Failed to load: %s\n", shared_blobs_filename);
        Dart_ExitScope();
        Dart_ShutdownIsolate();
        exit(kErrorExitCode);
      }
      const uint8_t* ignored;
      shared_blobs->SetBuffers(&ignored, &ignored, &shared_data,
                               &shared_instructions);
    } else {
      if (shared_data_filename != NULL) {
        mapped_shared_data =
            MapFile(shared_data_filename, File::kReadOnly, &shared_data);
      }
      if (shared_instructions_filename != NULL) {
        mapped_shared_instructions =
            MapFile(shared_instructions_filename, File::kReadOnly,
                    &shared_instructions);
      }
    }

    uint8_t* vm_snapshot_data_buffer = NULL;
    intptr_t vm_snapshot_data_size = 0;
    uint8_t* vm_snapshot_instructions_buffer = NULL;
    intptr_t vm_snapshot_instructions_size = 0;
    uint8_t* isolate_snapshot_data_buffer = NULL;
    intptr_t isolate_snapshot_data_size = 0;
    uint8_t* isolate_snapshot_instructions_buffer = NULL;
    intptr_t isolate_snapshot_instructions_size = 0;
    result = Dart_CreateAppAOTSnapshotAsBlobs(
        &vm_snapshot_data_buffer, &vm_snapshot_data_size,
        &vm_snapshot_instructions_buffer, &vm_snapshot_instructions_size,
        &isolate_snapshot_data_buffer, &isolate_snapshot_data_size,
        &isolate_snapshot_instructions_buffer,
        &isolate_snapshot_instructions_size, shared_data, shared_instructions);
    CHECK_RESULT(result);

    if (blobs_container_filename != NULL) {
      Snapshot::WriteAppSnapshot(
          blobs_container_filename, vm_snapshot_data_buffer,
          vm_snapshot_data_size, vm_snapshot_instructions_buffer,
          vm_snapshot_instructions_size, isolate_snapshot_data_buffer,
          isolate_snapshot_data_size, isolate_snapshot_instructions_buffer,
          isolate_snapshot_instructions_size);
    } else {
      WriteFile(vm_snapshot_data_filename, vm_snapshot_data_buffer,
                vm_snapshot_data_size);
      WriteFile(vm_snapshot_instructions_filename,
                vm_snapshot_instructions_buffer, vm_snapshot_instructions_size);
      WriteFile(isolate_snapshot_data_filename, isolate_snapshot_data_buffer,
                isolate_snapshot_data_size);
      WriteFile(isolate_snapshot_instructions_filename,
                isolate_snapshot_instructions_buffer,
                isolate_snapshot_instructions_size);
    }
  } else {
    UNREACHABLE();
  }

  // Serialize obfuscation map if requested.
  if (obfuscation_map_filename != NULL) {
    ASSERT(obfuscate);
    uint8_t* buffer = NULL;
    intptr_t size = 0;
    result = Dart_GetObfuscationMap(&buffer, &size);
    CHECK_RESULT(result);
    WriteFile(obfuscation_map_filename, buffer, size);
  }
}

static Dart_QualifiedFunctionName no_entry_points[] = {
    {NULL, NULL, NULL}  // Must be terminated with NULL entries.
};

static int CreateIsolateAndSnapshot(const CommandLineOptions& inputs) {
  uint8_t* kernel_buffer = NULL;
  intptr_t kernel_buffer_size = NULL;
  ReadFile(inputs.GetArgument(0), &kernel_buffer, &kernel_buffer_size);

  Dart_IsolateFlags isolate_flags;
  Dart_IsolateFlagsInitialize(&isolate_flags);
  if (IsSnapshottingForPrecompilation()) {
    isolate_flags.obfuscate = obfuscate;
    isolate_flags.entry_points = no_entry_points;
  }

  auto isolate_group_data = new IsolateGroupData(NULL, NULL, NULL, NULL);
  Dart_Isolate isolate;
  char* error = NULL;
  if (isolate_snapshot_data == NULL) {
    // We need to capture the vmservice library in the core snapshot, so load it
    // in the main isolate as well.
    isolate_flags.load_vmservice_library = true;
    isolate = Dart_CreateIsolateGroupFromKernel(
        NULL, NULL, kernel_buffer, kernel_buffer_size, &isolate_flags,
        isolate_group_data, /*isolate_data=*/nullptr, &error);
  } else {
    isolate = Dart_CreateIsolateGroup(NULL, NULL, isolate_snapshot_data,
                                      isolate_snapshot_instructions, NULL, NULL,
                                      &isolate_flags, isolate_group_data,
                                      /*isolate_data=*/nullptr, &error);
  }
  if (isolate == NULL) {
    delete isolate_group_data;
    Syslog::PrintErr("%s\n", error);
    free(error);
    return kErrorExitCode;
  }

  Dart_EnterScope();
  Dart_Handle result =
      Dart_SetEnvironmentCallback(DartUtils::EnvironmentCallback);
  CHECK_RESULT(result);

  // The root library has to be set to generate AOT snapshots, and sometimes we
  // set one for the core snapshot too.
  // If the input dill file has a root library, then Dart_LoadScript will
  // ignore this dummy uri and set the root library to the one reported in
  // the dill file. Since dill files are not dart script files,
  // trying to resolve the root library URI based on the dill file name
  // would not help.
  //
  // If the input dill file does not have a root library, then
  // Dart_LoadScript will error.
  //
  // TODO(kernel): Dart_CreateIsolateGroupFromKernel should respect the root
  // library in the kernel file, though this requires auditing the other
  // loading paths in the embedders that had to work around this.
  result = Dart_SetRootLibrary(
      Dart_LoadLibraryFromKernel(kernel_buffer, kernel_buffer_size));
  CHECK_RESULT(result);

  MaybeLoadExtraInputs(inputs);

  MaybeLoadCode();

  switch (snapshot_kind) {
    case kCore:
      CreateAndWriteCoreSnapshot();
      break;
    case kCoreJIT:
      CreateAndWriteCoreJITSnapshot();
      break;
    case kApp:
      CreateAndWriteAppSnapshot();
      break;
    case kAppJIT:
      CreateAndWriteAppJITSnapshot();
      break;
    case kAppAOTAssembly:
    case kAppAOTBlobs:
    case kAppAOTElf:
      CreateAndWritePrecompiledSnapshot();
      break;
    case kVMAOTAssembly: {
      File* file = OpenFile(assembly_filename);
      RefCntReleaseScope<File> rs(file);
      result = Dart_CreateVMAOTSnapshotAsAssembly(StreamingWriteCallback, file);
      CHECK_RESULT(result);
      break;
    }
    default:
      UNREACHABLE();
  }

  Dart_ExitScope();
  Dart_ShutdownIsolate();
  return 0;
}

int main(int argc, char** argv) {
  const int EXTRA_VM_ARGUMENTS = 7;
  CommandLineOptions vm_options(argc + EXTRA_VM_ARGUMENTS);
  CommandLineOptions inputs(argc);

  // When running from the command line we assume that we are optimizing for
  // throughput, and therefore use a larger new gen semi space size and a faster
  // new gen growth factor unless others have been specified.
  if (kWordSize <= 4) {
    vm_options.AddArgument("--new_gen_semi_max_size=16");
  } else {
    vm_options.AddArgument("--new_gen_semi_max_size=32");
  }
  vm_options.AddArgument("--new_gen_growth_factor=4");
  vm_options.AddArgument("--deterministic");

  // Parse command line arguments.
  if (ParseArguments(argc, argv, &vm_options, &inputs) < 0) {
    PrintUsage();
    return kErrorExitCode;
  }
  DartUtils::SetEnvironment(environment);

  if (!Platform::Initialize()) {
    Syslog::PrintErr("Initialization failed\n");
    return kErrorExitCode;
  }
  Console::SaveConfig();
  Loader::InitOnce();
  DartUtils::SetOriginalWorkingDirectory();
  // Start event handler.
  TimerUtils::InitOnce();
  EventHandler::Start();

#if !defined(PRODUCT)
  // Constant true in PRODUCT mode.
  vm_options.AddArgument("--load_deferred_eagerly");
#endif

  if (IsSnapshottingForPrecompilation()) {
    vm_options.AddArgument("--precompilation");
  } else if ((snapshot_kind == kCoreJIT) || (snapshot_kind == kAppJIT)) {
    vm_options.AddArgument("--fields_may_be_reset");
#if !defined(TARGET_ARCH_IA32)
    vm_options.AddArgument("--link_natives_lazily");
#endif
  }

  char* error = Dart_SetVMFlags(vm_options.count(), vm_options.arguments());
  if (error != NULL) {
    Syslog::PrintErr("Setting VM flags failed: %s\n", error);
    free(error);
    return kErrorExitCode;
  }

  Dart_InitializeParams init_params;
  memset(&init_params, 0, sizeof(init_params));
  init_params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  init_params.file_open = DartUtils::OpenFile;
  init_params.file_read = DartUtils::ReadFile;
  init_params.file_write = DartUtils::WriteFile;
  init_params.file_close = DartUtils::CloseFile;
  init_params.entropy_source = DartUtils::EntropySource;
  init_params.start_kernel_isolate = false;

  std::unique_ptr<MappedMemory> mapped_vm_snapshot_data;
  std::unique_ptr<MappedMemory> mapped_vm_snapshot_instructions;
  std::unique_ptr<MappedMemory> mapped_isolate_snapshot_data;
  std::unique_ptr<MappedMemory> mapped_isolate_snapshot_instructions;
  if (load_vm_snapshot_data_filename != NULL) {
    mapped_vm_snapshot_data =
        MapFile(load_vm_snapshot_data_filename, File::kReadOnly,
                &init_params.vm_snapshot_data);
  }
  if (load_vm_snapshot_instructions_filename != NULL) {
    mapped_vm_snapshot_instructions =
        MapFile(load_vm_snapshot_instructions_filename, File::kReadExecute,
                &init_params.vm_snapshot_instructions);
  }
  if (load_isolate_snapshot_data_filename) {
    mapped_isolate_snapshot_data =
        MapFile(load_isolate_snapshot_data_filename, File::kReadOnly,
                &isolate_snapshot_data);
  }
  if (load_isolate_snapshot_instructions_filename != NULL) {
    mapped_isolate_snapshot_instructions =
        MapFile(load_isolate_snapshot_instructions_filename, File::kReadExecute,
                &isolate_snapshot_instructions);
  }

  error = Dart_Initialize(&init_params);
  if (error != NULL) {
    Syslog::PrintErr("VM initialization failed: %s\n", error);
    free(error);
    return kErrorExitCode;
  }

  int result = CreateIsolateAndSnapshot(inputs);
  if (result != 0) {
    return result;
  }

  error = Dart_Cleanup();
  if (error != NULL) {
    Syslog::PrintErr("VM cleanup failed: %s\n", error);
    free(error);
  }
  EventHandler::Stop();
  return 0;
}

}  // namespace bin
}  // namespace dart

int main(int argc, char** argv) {
  return dart::bin::main(argc, argv);
}
