#include "project.h"

#include "clang_utils.h"
#include "match.h"
#include "platform.h"
#include "serializer.h"
#include "timer.h"
#include "utils.h"

#include <clang-c/CXCompilationDatabase.h>
#include <doctest/doctest.h>
#include <loguru.hpp>

#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <vector>

struct CompileCommandsEntry {
  std::string directory;
  std::string file;
  std::string command;
  std::vector<std::string> args;
};
MAKE_REFLECT_STRUCT(CompileCommandsEntry, directory, file, command, args);

namespace {

bool g_disable_normalize_path_for_test = false;

std::string NormalizePathWithTestOptOut(const std::string& path) {
  if (g_disable_normalize_path_for_test) {
    // Add a & so we can test to verify a path is normalized.
    return "&" + path;
  }
  return NormalizePath(path);
}

struct ProjectConfig {
  std::unordered_set<std::string> quote_dirs;
  std::unordered_set<std::string> angle_dirs;
  std::vector<std::string> extra_flags;
  std::string project_dir;
  std::string resource_dir;
};

// TODO: See
// https://github.com/Valloric/ycmd/blob/master/ycmd/completers/cpp/flags.py.
std::vector<std::string> kBlacklistMulti = {
    "-MF", "-MT", "-MQ", "-o", "--serialize-diagnostics", "-Xclang"};

// Blacklisted flags which are always removed from the command line.
std::vector<std::string> kBlacklist = {
    "-c", "-MP", "-MD", "-MMD", "--fcolor-diagnostics",
};

// Arguments which are followed by a potentially relative path. We need to make
// all relative paths absolute, otherwise libclang will not resolve them.
std::vector<std::string> kPathArgs = {
    "-I",        "-iquote",        "-isystem",     "--sysroot=",
    "-isysroot", "-gcc-toolchain", "-include-pch", "-iframework",
    "-F",        "-imacros",       "-include"};

// Arguments which always require an absolute path, ie, clang -working-directory
// does not work as expected. Argument processing assumes that this is a subset
// of kPathArgs.
std::vector<std::string> kNormalizePathArgs = {"--sysroot="};

// Arguments whose path arguments should be injected into include dir lookup
// for #include completion.
std::vector<std::string> kQuoteIncludeArgs = {"-iquote"};
std::vector<std::string> kAngleIncludeArgs = {"-I", "-isystem"};

bool ShouldAddToQuoteIncludes(const std::string& arg) {
  return StartsWithAny(arg, kQuoteIncludeArgs);
}
bool ShouldAddToAngleIncludes(const std::string& arg) {
  return StartsWithAny(arg, kAngleIncludeArgs);
}

// Returns true if we should use the C, not C++, language spec for the given
// file.
optional<std::string> SourceFileType(const std::string& path) {
  if (EndsWith(path, ".c"))
    return std::string("c");
  else if (EndsWith(path, ".cpp") || EndsWith(path, ".cc"))
    return std::string("c++");
  else if (EndsWith(path, ".mm"))
    return std::string("objective-c++");
  else if (EndsWith(path, ".m"))
    return std::string("objective-c");
  return nullopt;
}

Project::Entry GetCompilationEntryFromCompileCommandEntry(
    ProjectConfig* config,
    const CompileCommandsEntry& entry) {
  auto cleanup_maybe_relative_path = [&](const std::string& path) {
    // TODO/FIXME: Normalization will fail for paths that do not exist. Should
    // it return an optional<std::string>?
    assert(!path.empty());
    if (path[0] == '/' || entry.directory.empty())
      return NormalizePathWithTestOptOut(path);
    return NormalizePathWithTestOptOut(entry.directory + "/" + path);
  };

  Project::Entry result;
  result.filename = NormalizePathWithTestOptOut(entry.file);

  size_t i = 0;

  // Strip all arguments consisting the compiler command,
  // as there may be non-compiler related commands beforehand,
  // ie, compiler schedular such as goma. This allows correct parsing for
  // command lines like "goma clang -c foo".
  std::string::size_type dot;
  while (i < entry.args.size() && entry.args[i][0] != '-' &&
         // Do not skip over main source filename
         NormalizePathWithTestOptOut(entry.args[i]) != result.filename &&
         // There may be other filenames (e.g. more than one source filenames)
         // preceding main source filename. We use a heuristic here. `.` may
         // occur in both command names and source filenames. If `.` occurs in
         // the last 4 bytes of entry.args[i] and not followed by a digit, e.g.
         // .c .cpp, We take it as a source filename. Others (like ./a/b/goma
         // clang-4.0) are seen as commands.
         ((dot = entry.args[i].rfind('.')) == std::string::npos ||
          dot + 4 < entry.args[i].size() || isdigit(entry.args[i][dot + 1])))
    ++i;
  // Include the compiler in the args.
  if (i > 0)
    result.args.push_back(entry.args[i - 1]);
  else {
    // TODO Drop this back compatibility
    // Args probably came from a /.cquery file, which likely has just flags.
    // clang_parseTranslationUnit2FullArgv() expects the binary path as the
    // first arg, so the first flag would end up being ignored. Add a dummy.
    result.args.push_back("clang++");
  }

  if (!AnyStartsWith(entry.args, "-working-directory")) {
    result.args.emplace_back("-working-directory");
    result.args.push_back(entry.directory);
  }

  // Clang does not have good hueristics for determining source language, we
  // should explicitly specify it.
  if (auto source_file_type = SourceFileType(entry.file)) {
    if (!AnyStartsWith(entry.args, "-x")) {
      result.args.push_back("-x" + *source_file_type);
    }
    if (!AnyStartsWith(entry.args, "-std=")) {
      if (*source_file_type == "c")
        result.args.push_back("-std=gnu11");
      else if (*source_file_type == "c++")
        result.args.push_back("-std=c++14");
    }
  }

  bool next_flag_is_path = false;
  bool add_next_flag_to_quote_dirs = false;
  bool add_next_flag_to_angle_dirs = false;

  // Note that when processing paths, some arguments support multiple forms, ie,
  // {"-Ifoo"} or {"-I", "foo"}.  Support both styles.

  result.args.reserve(entry.args.size() + config->extra_flags.size());
  for (; i < entry.args.size(); ++i) {
    std::string arg = entry.args[i];

    // If blacklist skip.
    if (!next_flag_is_path) {
      if (StartsWithAny(arg, kBlacklistMulti)) {
        ++i;
        continue;
      }
      if (StartsWithAny(arg, kBlacklist))
        continue;
    }

    // Finish processing path for the previous argument, which was a switch.
    // {"-I", "foo"} style.
    if (next_flag_is_path) {
      std::string normalized_arg = cleanup_maybe_relative_path(arg);
      if (add_next_flag_to_quote_dirs)
        config->quote_dirs.insert(normalized_arg);
      if (add_next_flag_to_angle_dirs)
        config->angle_dirs.insert(normalized_arg);

      next_flag_is_path = false;
      add_next_flag_to_quote_dirs = false;
      add_next_flag_to_angle_dirs = false;
    }

    else {
      // Check to see if arg is a path and needs to be updated.
      for (const std::string& flag_type : kPathArgs) {
        // {"-I", "foo"} style.
        if (arg == flag_type) {
          next_flag_is_path = true;
          add_next_flag_to_quote_dirs = ShouldAddToQuoteIncludes(arg);
          add_next_flag_to_angle_dirs = ShouldAddToAngleIncludes(arg);
          break;
        }

        // {"-Ifoo"} style.
        if (StartsWith(arg, flag_type)) {
          std::string path = arg.substr(flag_type.size());
          assert(!path.empty());
          path = cleanup_maybe_relative_path(path);
          if (StartsWithAny(arg, kNormalizePathArgs))
            arg = flag_type + path;
          if (ShouldAddToQuoteIncludes(flag_type))
            config->quote_dirs.insert(path);
          if (ShouldAddToAngleIncludes(flag_type))
            config->angle_dirs.insert(path);
          break;
        }
      }
    }

    result.args.push_back(arg);
  }

  // We don't do any special processing on user-given extra flags.
  for (const auto& flag : config->extra_flags)
    result.args.push_back(flag);

  // Add -resource-dir so clang can correctly resolve system includes like
  // <cstddef>
  if (!AnyStartsWith(result.args, "-resource-dir"))
    result.args.push_back("-resource-dir=" + config->resource_dir);

  // There could be a clang version mismatch between what the project uses and
  // what cquery uses. Make sure we do not emit warnings for mismatched options.
  if (!AnyStartsWith(result.args, "-Wno-unknown-warning-option"))
    result.args.push_back("-Wno-unknown-warning-option");

  // Using -fparse-all-comments enables documententation in the indexer and in
  // code completion.
  if (!AnyStartsWith(result.args, "-fparse-all-comments"))
    result.args.push_back("-fparse-all-comments");

  return result;
}

std::vector<Project::Entry> LoadFromDirectoryListing(ProjectConfig* config) {
  std::vector<Project::Entry> result;

  std::vector<std::string> args;
  for (std::string line :
       ReadLinesWithEnding(config->project_dir + "/.cquery")) {
    TrimInPlace(line);
    if (line.empty() || StartsWith(line, "#"))
      continue;
    args.push_back(line);
  }
  LOG_IF_S(INFO, !args.empty())
      << "Using .cquery arguments " << StringJoin(args);
  LOG_IF_S(WARNING, !FileExists(config->project_dir + "/.cquery") &&
                        config->extra_flags.empty())
      << "cquery has no clang arguments. Considering adding either a "
         "compile_commands.json or .cquery file. See the cquery README for "
         "more information.";

  std::vector<std::string> files = GetFilesInFolder(
      config->project_dir, true /*recursive*/, true /*add_folder_to_path*/);
  for (const std::string& file : files) {
    if (SourceFileType(file)) {
      CompileCommandsEntry e;
      e.directory = config->project_dir;
      e.file = file;
      e.args = args;
      e.args.push_back(e.file);
      result.push_back(GetCompilationEntryFromCompileCommandEntry(config, e));
    }
  }

  return result;
}

std::vector<Project::Entry> LoadCompilationEntriesFromDirectory(
    ProjectConfig* config,
    const std::string& opt_compilation_db_dir) {
  // If there is a .cquery file always load using directory listing.
  if (FileExists(config->project_dir + "/.cquery"))
    return LoadFromDirectoryListing(config);

  // Try to load compile_commands.json, but fallback to a project listing.
  const auto& compilation_db_dir = opt_compilation_db_dir.empty()
                                       ? config->project_dir
                                       : opt_compilation_db_dir;
  LOG_S(INFO) << "Trying to load compile_commands.json";
  CXCompilationDatabase_Error cx_db_load_error;
  CXCompilationDatabase cx_db = clang_CompilationDatabase_fromDirectory(
      compilation_db_dir.c_str(), &cx_db_load_error);
  if (cx_db_load_error == CXCompilationDatabase_CanNotLoadDatabase) {
    LOG_S(INFO) << "Unable to load compile_commands.json located at \""
                << compilation_db_dir << "\"; using directory listing instead.";
    return LoadFromDirectoryListing(config);
  }

  Timer clang_time;
  Timer our_time;
  clang_time.Pause();
  our_time.Pause();

  clang_time.Resume();
  CXCompileCommands cx_commands =
      clang_CompilationDatabase_getAllCompileCommands(cx_db);
  unsigned int num_commands = clang_CompileCommands_getSize(cx_commands);
  clang_time.Pause();

  std::vector<Project::Entry> result;
  for (unsigned int i = 0; i < num_commands; i++) {
    clang_time.Resume();
    CXCompileCommand cx_command =
        clang_CompileCommands_getCommand(cx_commands, i);

    std::string directory =
        ToString(clang_CompileCommand_getDirectory(cx_command));
    std::string relative_filename =
        ToString(clang_CompileCommand_getFilename(cx_command));

    unsigned num_args = clang_CompileCommand_getNumArgs(cx_command);
    CompileCommandsEntry entry;
    entry.args.reserve(num_args);
    for (unsigned j = 0; j < num_args; ++j) {
      entry.args.push_back(
          ToString(clang_CompileCommand_getArg(cx_command, j)));
    }
    clang_time.Pause();  // TODO: don't call ToString in this block.
    // LOG_S(INFO) << "Got args " << StringJoin(entry.args);

    our_time.Resume();
    entry.directory = directory;
    std::string absolute_filename;
    if (!relative_filename.empty() && relative_filename[0] == '/')
      absolute_filename = relative_filename;
    else
      absolute_filename = directory + "/" + relative_filename;
    entry.file = NormalizePathWithTestOptOut(absolute_filename);

    result.push_back(GetCompilationEntryFromCompileCommandEntry(config, entry));
    our_time.Pause();
  }

  clang_time.Resume();
  clang_CompileCommands_dispose(cx_commands);
  clang_CompilationDatabase_dispose(cx_db);
  clang_time.Pause();

  clang_time.ResetAndPrint("compile_commands.json clang time");
  our_time.ResetAndPrint("compile_commands.json our time");

  return result;
}

// Computes a score based on how well |a| and |b| match. This is used for
// argument guessing.
int ComputeGuessScore(const std::string& a, const std::string& b) {
  const int kMatchPrefixWeight = 100;
  const int kMismatchDirectoryWeight = 100;
  const int kMatchPostfixWeight = 1;

  int score = 0;
  int i = 0;

  // Increase score based on matching prefix.
  for (i = 0; i < a.length() && i < b.length(); ++i) {
    if (a[i] != b[i])
      break;
    score += kMatchPrefixWeight;
  }

  // Reduce score based on mismatched directory distance.
  for (int j = i; j < a.length(); ++j) {
    if (a[j] == '/')
      score -= kMismatchDirectoryWeight;
  }
  for (int j = i; j < b.length(); ++j) {
    if (b[j] == '/')
      score -= kMismatchDirectoryWeight;
  }

  // Increase score based on common ending. Don't increase as much as matching
  // prefix or directory distance.
  for (int offset = 1; offset <= a.length() && offset <= b.length(); ++offset) {
    if (a[a.size() - offset] != b[b.size() - offset])
      break;
    score += kMatchPostfixWeight;
  }

  return score;
}

}  // namespace

void Project::Load(const std::vector<std::string>& extra_flags,
                   const std::string& opt_compilation_db_dir,
                   const std::string& root_directory,
                   const std::string& resource_directory) {
  // Load data.
  ProjectConfig config;
  config.extra_flags = extra_flags;
  config.project_dir = root_directory;
  config.resource_dir = resource_directory;
  entries =
      LoadCompilationEntriesFromDirectory(&config, opt_compilation_db_dir);

  // Cleanup / postprocess include directories.
  quote_include_directories.assign(config.quote_dirs.begin(),
                                   config.quote_dirs.end());
  angle_include_directories.assign(config.angle_dirs.begin(),
                                   config.angle_dirs.end());
  for (std::string& path : quote_include_directories) {
    EnsureEndsInSlash(path);
    LOG_S(INFO) << "quote_include_dir: " << path;
  }
  for (std::string& path : angle_include_directories) {
    EnsureEndsInSlash(path);
    LOG_S(INFO) << "angle_include_dir: " << path;
  }

  // Setup project entries.
  absolute_path_to_entry_index_.resize(entries.size());
  for (int i = 0; i < entries.size(); ++i)
    absolute_path_to_entry_index_[entries[i].filename] = i;
}

Project::Entry Project::FindCompilationEntryForFile(
    const std::string& filename) {
  auto it = absolute_path_to_entry_index_.find(filename);
  if (it != absolute_path_to_entry_index_.end())
    return entries[it->second];

  // We couldn't find the file. Try to infer it.
  // TODO: Cache inferred file in a separate array (using a lock or similar)
  Entry* best_entry = nullptr;
  int best_score = std::numeric_limits<int>::min();
  for (Entry& entry : entries) {
    int score = ComputeGuessScore(filename, entry.filename);
    if (score > best_score) {
      best_score = score;
      best_entry = &entry;
    }
  }

  Project::Entry result;
  result.is_inferred = true;
  result.filename = filename;
  if (best_entry)
    result.args = best_entry->args;
  return result;
}

void Project::ForAllFilteredFiles(
    Config* config,
    std::function<void(int i, const Entry& entry)> action) {
  GroupMatch matcher(config->indexWhitelist, config->indexBlacklist);
  for (int i = 0; i < entries.size(); ++i) {
    const Project::Entry& entry = entries[i];
    std::string failure_reason;
    if (matcher.IsMatch(entry.filename, &failure_reason))
      action(i, entries[i]);
    else {
      if (config->logSkippedPathsForIndex) {
        LOG_S(INFO) << "[" << i + 1 << "/" << entries.size() << "]: Failed "
                    << failure_reason << "; skipping " << entry.filename;
      }
    }
  }
}

TEST_SUITE("Project") {
  void CheckFlags(const std::string& directory, const std::string& file,
                  std::vector<std::string> raw,
                  std::vector<std::string> expected) {
    g_disable_normalize_path_for_test = true;

    ProjectConfig config;
    config.project_dir = "/w/c/s/";
    config.resource_dir = "/w/resource_dir/";

    CompileCommandsEntry entry;
    entry.directory = directory;
    entry.args = raw;
    entry.file = file;
    Project::Entry result =
        GetCompilationEntryFromCompileCommandEntry(&config, entry);

    if (result.args != expected) {
      std::cout << "Raw:      " << StringJoin(raw) << std::endl;
      std::cout << "Expected: " << StringJoin(expected) << std::endl;
      std::cout << "Actual:   " << StringJoin(result.args) << std::endl;
    }
    for (int i = 0; i < std::min(result.args.size(), expected.size()); ++i) {
      if (result.args[i] != expected[i]) {
        std::cout << std::endl;
        std::cout << "mismatch at " << i << std::endl;
        std::cout << "  expected: " << expected[i] << std::endl;
        std::cout << "  actual:   " << result.args[i] << std::endl;
      }
    }
    REQUIRE(result.args == expected);
  }

  void CheckFlags(std::vector<std::string> raw,
                  std::vector<std::string> expected) {
    CheckFlags("/dir/", "file.cc", raw, expected);
  }

  TEST_CASE("strip meta-compiler invocations") {
    CheckFlags(
        /* raw */ {"clang", "-lstdc++", "myfile.cc"},
        /* expected */
        {"clang", "-working-directory", "/dir/", "-xc++", "-std=c++14",
         "-lstdc++", "myfile.cc", "-resource-dir=/w/resource_dir/",
         "-Wno-unknown-warning-option", "-fparse-all-comments"});

    CheckFlags(
        /* raw */ {"goma", "clang"},
        /* expected */
        {"clang", "-working-directory", "/dir/", "-xc++", "-std=c++14",
         "-resource-dir=/w/resource_dir/", "-Wno-unknown-warning-option",
         "-fparse-all-comments"});

    CheckFlags(
        /* raw */ {"goma", "clang", "--foo"},
        /* expected */
        {"clang", "-working-directory", "/dir/", "-xc++", "-std=c++14", "--foo",
         "-resource-dir=/w/resource_dir/", "-Wno-unknown-warning-option",
         "-fparse-all-comments"});
  }

  // FIXME: Fix this test.
  TEST_CASE("Path in args") {
    CheckFlags("/home/user", "/home/user/foo/bar.c",
               /* raw */ {"cc", "-O0", "foo/bar.c"},
               /* expected */
               {"cc", "-working-directory", "/home/user", "-xc", "-std=gnu11",
                "-O0", "foo/bar.c", "-resource-dir=/w/resource_dir/",
                "-Wno-unknown-warning-option", "-fparse-all-comments"});
  }

  TEST_CASE("Implied binary") {
    CheckFlags(
        "/home/user", "/home/user/foo/bar.cc",
        /* raw */ {"-DDONT_IGNORE_ME"},
        /* expected */
        {"clang++", "-working-directory", "/home/user", "-xc++", "-std=c++14",
         "-DDONT_IGNORE_ME", "-resource-dir=/w/resource_dir/",
         "-Wno-unknown-warning-option", "-fparse-all-comments"});
  }

  // Checks flag parsing for a random chromium file in comparison to what
  // YouCompleteMe fetches.
  TEST_CASE("ycm") {
    CheckFlags(
        "/w/c/s/out/Release", "../../ash/login/lock_screen_sanity_unittest.cc",

        /* raw */
        {
            "/work/goma/gomacc",
            "../../third_party/llvm-build/Release+Asserts/bin/clang++",
            "-MMD",
            "-MF",
            "obj/ash/ash_unittests/lock_screen_sanity_unittest.o.d",
            "-DV8_DEPRECATION_WARNINGS",
            "-DDCHECK_ALWAYS_ON=1",
            "-DUSE_UDEV",
            "-DUSE_AURA=1",
            "-DUSE_NSS_CERTS=1",
            "-DUSE_OZONE=1",
            "-DFULL_SAFE_BROWSING",
            "-DSAFE_BROWSING_CSD",
            "-DSAFE_BROWSING_DB_LOCAL",
            "-DCHROMIUM_BUILD",
            "-DFIELDTRIAL_TESTING_ENABLED",
            "-D_FILE_OFFSET_BITS=64",
            "-D_LARGEFILE_SOURCE",
            "-D_LARGEFILE64_SOURCE",
            "-DCR_CLANG_REVISION=\"313786-1\"",
            "-D__STDC_CONSTANT_MACROS",
            "-D__STDC_FORMAT_MACROS",
            "-DCOMPONENT_BUILD",
            "-DOS_CHROMEOS",
            "-DNDEBUG",
            "-DNVALGRIND",
            "-DDYNAMIC_ANNOTATIONS_ENABLED=0",
            "-DGL_GLEXT_PROTOTYPES",
            "-DUSE_GLX",
            "-DUSE_EGL",
            "-DANGLE_ENABLE_RELEASE_ASSERTS",
            "-DTOOLKIT_VIEWS=1",
            "-DGTEST_API_=",
            "-DGTEST_HAS_POSIX_RE=0",
            "-DGTEST_LANG_CXX11=1",
            "-DUNIT_TEST",
            "-DUSING_V8_SHARED",
            "-DU_USING_ICU_NAMESPACE=0",
            "-DU_ENABLE_DYLOAD=0",
            "-DICU_UTIL_DATA_IMPL=ICU_UTIL_DATA_FILE",
            "-DUCHAR_TYPE=uint16_t",
            "-DGOOGLE_PROTOBUF_NO_RTTI",
            "-DGOOGLE_PROTOBUF_NO_STATIC_INITIALIZER",
            "-DHAVE_PTHREAD",
            "-DPROTOBUF_USE_DLLS",
            "-DBORINGSSL_SHARED_LIBRARY",
            "-DSK_IGNORE_LINEONLY_AA_CONVEX_PATH_OPTS",
            "-DSK_HAS_PNG_LIBRARY",
            "-DSK_HAS_WEBP_LIBRARY",
            "-DSK_HAS_JPEG_LIBRARY",
            "-DSKIA_DLL",
            "-DGR_GL_IGNORE_ES3_MSAA=0",
            "-DSK_SUPPORT_GPU=1",
            "-DMESA_EGL_NO_X11_HEADERS",
            "-I../..",
            "-Igen",
            "-I../../third_party/libwebp/src",
            "-I../../third_party/khronos",
            "-I../../gpu",
            "-I../../third_party/googletest/src/googletest/include",
            "-I../../third_party/WebKit",
            "-Igen/third_party/WebKit",
            "-I../../v8/include",
            "-Igen/v8/include",
            "-I../../third_party/icu/source/common",
            "-I../../third_party/icu/source/i18n",
            "-I../../third_party/protobuf/src",
            "-Igen/protoc_out",
            "-I../../third_party/protobuf/src",
            "-I../../third_party/boringssl/src/include",
            "-I../../build/linux/debian_jessie_amd64-sysroot/usr/include/nss",
            "-I../../build/linux/debian_jessie_amd64-sysroot/usr/include/nspr",
            "-I../../skia/config",
            "-I../../skia/ext",
            "-I../../third_party/skia/include/c",
            "-I../../third_party/skia/include/config",
            "-I../../third_party/skia/include/core",
            "-I../../third_party/skia/include/effects",
            "-I../../third_party/skia/include/encode",
            "-I../../third_party/skia/include/gpu",
            "-I../../third_party/skia/include/images",
            "-I../../third_party/skia/include/lazy",
            "-I../../third_party/skia/include/pathops",
            "-I../../third_party/skia/include/pdf",
            "-I../../third_party/skia/include/pipe",
            "-I../../third_party/skia/include/ports",
            "-I../../third_party/skia/include/utils",
            "-I../../third_party/skia/third_party/vulkan",
            "-I../../third_party/skia/include/codec",
            "-I../../third_party/skia/src/gpu",
            "-I../../third_party/skia/src/sksl",
            "-I../../third_party/ced/src",
            "-I../../third_party/mesa/src/include",
            "-I../../third_party/libwebm/source",
            "-Igen",
            "-I../../build/linux/debian_jessie_amd64-sysroot/usr/include/"
            "dbus-1.0",
            "-I../../build/linux/debian_jessie_amd64-sysroot/usr/lib/"
            "x86_64-linux-gnu/dbus-1.0/include",
            "-I../../third_party/googletest/custom",
            "-I../../third_party/googletest/src/googlemock/include",
            "-fno-strict-aliasing",
            "-Wno-builtin-macro-redefined",
            "-D__DATE__=",
            "-D__TIME__=",
            "-D__TIMESTAMP__=",
            "-funwind-tables",
            "-fPIC",
            "-pipe",
            "-B../../third_party/binutils/Linux_x64/Release/bin",
            "-pthread",
            "-fcolor-diagnostics",
            "-no-canonical-prefixes",
            "-m64",
            "-march=x86-64",
            "-Wall",
            "-Werror",
            "-Wextra",
            "-Wno-missing-field-initializers",
            "-Wno-unused-parameter",
            "-Wno-c++11-narrowing",
            "-Wno-covered-switch-default",
            "-Wno-unneeded-internal-declaration",
            "-Wno-inconsistent-missing-override",
            "-Wno-undefined-var-template",
            "-Wno-nonportable-include-path",
            "-Wno-address-of-packed-member",
            "-Wno-unused-lambda-capture",
            "-Wno-user-defined-warnings",
            "-Wno-enum-compare-switch",
            "-Wno-tautological-unsigned-zero-compare",
            "-Wno-null-pointer-arithmetic",
            "-Wno-tautological-unsigned-enum-zero-compare",
            "-O2",
            "-fno-ident",
            "-fdata-sections",
            "-ffunction-sections",
            "-fno-omit-frame-pointer",
            "-g0",
            "-fvisibility=hidden",
            "-Xclang",
            "-load",
            "-Xclang",
            "../../third_party/llvm-build/Release+Asserts/lib/"
            "libFindBadConstructs.so",
            "-Xclang",
            "-add-plugin",
            "-Xclang",
            "find-bad-constructs",
            "-Xclang",
            "-plugin-arg-find-bad-constructs",
            "-Xclang",
            "check-auto-raw-pointer",
            "-Xclang",
            "-plugin-arg-find-bad-constructs",
            "-Xclang",
            "check-ipc",
            "-Wheader-hygiene",
            "-Wstring-conversion",
            "-Wtautological-overlap-compare",
            "-Wno-header-guard",
            "-std=gnu++14",
            "-fno-rtti",
            "-nostdinc++",
            "-isystem../../buildtools/third_party/libc++/trunk/include",
            "-isystem../../buildtools/third_party/libc++abi/trunk/include",
            "--sysroot=../../build/linux/debian_jessie_amd64-sysroot",
            "-fno-exceptions",
            "-fvisibility-inlines-hidden",
            "-c",
            "../../ash/login/ui/lock_screen_sanity_unittest.cc",
            "-o",
            "obj/ash/ash_unittests/lock_screen_sanity_unittest.o",
        },

        /* expected */
        {"../../third_party/llvm-build/Release+Asserts/bin/clang++",
         "-working-directory",
         "/w/c/s/out/Release",
         "-xc++",
         "-DV8_DEPRECATION_WARNINGS",
         "-DDCHECK_ALWAYS_ON=1",
         "-DUSE_UDEV",
         "-DUSE_AURA=1",
         "-DUSE_NSS_CERTS=1",
         "-DUSE_OZONE=1",
         "-DFULL_SAFE_BROWSING",
         "-DSAFE_BROWSING_CSD",
         "-DSAFE_BROWSING_DB_LOCAL",
         "-DCHROMIUM_BUILD",
         "-DFIELDTRIAL_TESTING_ENABLED",
         "-D_FILE_OFFSET_BITS=64",
         "-D_LARGEFILE_SOURCE",
         "-D_LARGEFILE64_SOURCE",
         "-DCR_CLANG_REVISION=\"313786-1\"",
         "-D__STDC_CONSTANT_MACROS",
         "-D__STDC_FORMAT_MACROS",
         "-DCOMPONENT_BUILD",
         "-DOS_CHROMEOS",
         "-DNDEBUG",
         "-DNVALGRIND",
         "-DDYNAMIC_ANNOTATIONS_ENABLED=0",
         "-DGL_GLEXT_PROTOTYPES",
         "-DUSE_GLX",
         "-DUSE_EGL",
         "-DANGLE_ENABLE_RELEASE_ASSERTS",
         "-DTOOLKIT_VIEWS=1",
         "-DGTEST_API_=",
         "-DGTEST_HAS_POSIX_RE=0",
         "-DGTEST_LANG_CXX11=1",
         "-DUNIT_TEST",
         "-DUSING_V8_SHARED",
         "-DU_USING_ICU_NAMESPACE=0",
         "-DU_ENABLE_DYLOAD=0",
         "-DICU_UTIL_DATA_IMPL=ICU_UTIL_DATA_FILE",
         "-DUCHAR_TYPE=uint16_t",
         "-DGOOGLE_PROTOBUF_NO_RTTI",
         "-DGOOGLE_PROTOBUF_NO_STATIC_INITIALIZER",
         "-DHAVE_PTHREAD",
         "-DPROTOBUF_USE_DLLS",
         "-DBORINGSSL_SHARED_LIBRARY",
         "-DSK_IGNORE_LINEONLY_AA_CONVEX_PATH_OPTS",
         "-DSK_HAS_PNG_LIBRARY",
         "-DSK_HAS_WEBP_LIBRARY",
         "-DSK_HAS_JPEG_LIBRARY",
         "-DSKIA_DLL",
         "-DGR_GL_IGNORE_ES3_MSAA=0",
         "-DSK_SUPPORT_GPU=1",
         "-DMESA_EGL_NO_X11_HEADERS",
         "-I../..",
         "-Igen",
         "-I../../third_party/libwebp/src",
         "-I../../third_party/khronos",
         "-I../../gpu",
         "-I../../third_party/googletest/src/googletest/"
         "include",
         "-I../../third_party/WebKit",
         "-Igen/third_party/WebKit",
         "-I../../v8/include",
         "-Igen/v8/include",
         "-I../../third_party/icu/source/common",
         "-I../../third_party/icu/source/i18n",
         "-I../../third_party/protobuf/src",
         "-Igen/protoc_out",
         "-I../../third_party/protobuf/src",
         "-I../../third_party/boringssl/src/include",
         "-I../../build/linux/debian_jessie_amd64-sysroot/"
         "usr/include/nss",
         "-I../../build/linux/debian_jessie_amd64-sysroot/"
         "usr/include/nspr",
         "-I../../skia/config",
         "-I../../skia/ext",
         "-I../../third_party/skia/include/c",
         "-I../../third_party/skia/include/config",
         "-I../../third_party/skia/include/core",
         "-I../../third_party/skia/include/effects",
         "-I../../third_party/skia/include/encode",
         "-I../../third_party/skia/include/gpu",
         "-I../../third_party/skia/include/images",
         "-I../../third_party/skia/include/lazy",
         "-I../../third_party/skia/include/pathops",
         "-I../../third_party/skia/include/pdf",
         "-I../../third_party/skia/include/pipe",
         "-I../../third_party/skia/include/ports",
         "-I../../third_party/skia/include/utils",
         "-I../../third_party/skia/third_party/vulkan",
         "-I../../third_party/skia/include/codec",
         "-I../../third_party/skia/src/gpu",
         "-I../../third_party/skia/src/sksl",
         "-I../../third_party/ced/src",
         "-I../../third_party/mesa/src/include",
         "-I../../third_party/libwebm/source",
         "-Igen",
         "-I../../build/linux/debian_jessie_amd64-sysroot/"
         "usr/include/dbus-1.0",
         "-I../../build/linux/debian_jessie_amd64-sysroot/"
         "usr/lib/x86_64-linux-gnu/dbus-1.0/include",
         "-I../../third_party/googletest/custom",
         "-I../../third_party/googletest/src/googlemock/"
         "include",
         "-fno-strict-aliasing",
         "-Wno-builtin-macro-redefined",
         "-D__DATE__=",
         "-D__TIME__=",
         "-D__TIMESTAMP__=",
         "-funwind-tables",
         "-fPIC",
         "-pipe",
         "-B../../third_party/binutils/Linux_x64/Release/bin",
         "-pthread",
         "-fcolor-diagnostics",
         "-no-canonical-prefixes",
         "-m64",
         "-march=x86-64",
         "-Wall",
         "-Werror",
         "-Wextra",
         "-Wno-missing-field-initializers",
         "-Wno-unused-parameter",
         "-Wno-c++11-narrowing",
         "-Wno-covered-switch-default",
         "-Wno-unneeded-internal-declaration",
         "-Wno-inconsistent-missing-override",
         "-Wno-undefined-var-template",
         "-Wno-nonportable-include-path",
         "-Wno-address-of-packed-member",
         "-Wno-unused-lambda-capture",
         "-Wno-user-defined-warnings",
         "-Wno-enum-compare-switch",
         "-Wno-tautological-unsigned-zero-compare",
         "-Wno-null-pointer-arithmetic",
         "-Wno-tautological-unsigned-enum-zero-compare",
         "-O2",
         "-fno-ident",
         "-fdata-sections",
         "-ffunction-sections",
         "-fno-omit-frame-pointer",
         "-g0",
         "-fvisibility=hidden",
         "-Wheader-hygiene",
         "-Wstring-conversion",
         "-Wtautological-overlap-compare",
         "-Wno-header-guard",
         "-std=gnu++14",
         "-fno-rtti",
         "-nostdinc++",
         "-isystem../../buildtools/third_party/libc++/"
         "trunk/"
         "include",
         "-isystem../../buildtools/third_party/libc++abi/"
         "trunk/"
         "include",
         "--sysroot=&/w/c/s/out/Release/../../build/linux/"
         "debian_jessie_amd64-sysroot",
         "-fno-exceptions",
         "-fvisibility-inlines-hidden",
         "../../ash/login/ui/lock_screen_sanity_unittest.cc",
         "-resource-dir=/w/resource_dir/",
         "-Wno-unknown-warning-option",
         "-fparse-all-comments"});
  }

  // Checks flag parsing for an example chromium file.
  TEST_CASE("chromium") {
    CheckFlags(
        "/w/c/s/out/Release", "../../apps/app_lifetime_monitor.cc",
        /* raw */
        {"/work/goma/gomacc",
         "../../third_party/llvm-build/Release+Asserts/bin/clang++",
         "-MMD",
         "-MF",
         "obj/apps/apps/app_lifetime_monitor.o.d",
         "-DV8_DEPRECATION_WARNINGS",
         "-DDCHECK_ALWAYS_ON=1",
         "-DUSE_UDEV",
         "-DUSE_ASH=1",
         "-DUSE_AURA=1",
         "-DUSE_NSS_CERTS=1",
         "-DUSE_OZONE=1",
         "-DDISABLE_NACL",
         "-DFULL_SAFE_BROWSING",
         "-DSAFE_BROWSING_CSD",
         "-DSAFE_BROWSING_DB_LOCAL",
         "-DCHROMIUM_BUILD",
         "-DFIELDTRIAL_TESTING_ENABLED",
         "-DCR_CLANG_REVISION=\"310694-1\"",
         "-D_FILE_OFFSET_BITS=64",
         "-D_LARGEFILE_SOURCE",
         "-D_LARGEFILE64_SOURCE",
         "-D__STDC_CONSTANT_MACROS",
         "-D__STDC_FORMAT_MACROS",
         "-DCOMPONENT_BUILD",
         "-DOS_CHROMEOS",
         "-DNDEBUG",
         "-DNVALGRIND",
         "-DDYNAMIC_ANNOTATIONS_ENABLED=0",
         "-DGL_GLEXT_PROTOTYPES",
         "-DUSE_GLX",
         "-DUSE_EGL",
         "-DANGLE_ENABLE_RELEASE_ASSERTS",
         "-DTOOLKIT_VIEWS=1",
         "-DV8_USE_EXTERNAL_STARTUP_DATA",
         "-DU_USING_ICU_NAMESPACE=0",
         "-DU_ENABLE_DYLOAD=0",
         "-DICU_UTIL_DATA_IMPL=ICU_UTIL_DATA_FILE",
         "-DUCHAR_TYPE=uint16_t",
         "-DGOOGLE_PROTOBUF_NO_RTTI",
         "-DGOOGLE_PROTOBUF_NO_STATIC_INITIALIZER",
         "-DHAVE_PTHREAD",
         "-DPROTOBUF_USE_DLLS",
         "-DSK_IGNORE_LINEONLY_AA_CONVEX_PATH_OPTS",
         "-DSK_HAS_PNG_LIBRARY",
         "-DSK_HAS_WEBP_LIBRARY",
         "-DSK_HAS_JPEG_LIBRARY",
         "-DSKIA_DLL",
         "-DGR_GL_IGNORE_ES3_MSAA=0",
         "-DSK_SUPPORT_GPU=1",
         "-DMESA_EGL_NO_X11_HEADERS",
         "-DBORINGSSL_SHARED_LIBRARY",
         "-DUSING_V8_SHARED",
         "-I../..",
         "-Igen",
         "-I../../third_party/libwebp/src",
         "-I../../third_party/khronos",
         "-I../../gpu",
         "-I../../third_party/ced/src",
         "-I../../third_party/icu/source/common",
         "-I../../third_party/icu/source/i18n",
         "-I../../third_party/protobuf/src",
         "-I../../skia/config",
         "-I../../skia/ext",
         "-I../../third_party/skia/include/c",
         "-I../../third_party/skia/include/config",
         "-I../../third_party/skia/include/core",
         "-I../../third_party/skia/include/effects",
         "-I../../third_party/skia/include/encode",
         "-I../../third_party/skia/include/gpu",
         "-I../../third_party/skia/include/images",
         "-I../../third_party/skia/include/lazy",
         "-I../../third_party/skia/include/pathops",
         "-I../../third_party/skia/include/pdf",
         "-I../../third_party/skia/include/pipe",
         "-I../../third_party/skia/include/ports",
         "-I../../third_party/skia/include/utils",
         "-I../../third_party/skia/third_party/vulkan",
         "-I../../third_party/skia/src/gpu",
         "-I../../third_party/skia/src/sksl",
         "-I../../third_party/mesa/src/include",
         "-I../../third_party/libwebm/source",
         "-I../../third_party/protobuf/src",
         "-Igen/protoc_out",
         "-I../../third_party/boringssl/src/include",
         "-I../../build/linux/debian_jessie_amd64-sysroot/usr/include/nss",
         "-I../../build/linux/debian_jessie_amd64-sysroot/usr/include/nspr",
         "-Igen",
         "-I../../third_party/WebKit",
         "-Igen/third_party/WebKit",
         "-I../../v8/include",
         "-Igen/v8/include",
         "-Igen",
         "-I../../third_party/flatbuffers/src/include",
         "-Igen",
         "-fno-strict-aliasing",
         "-Wno-builtin-macro-redefined",
         "-D__DATE__=",
         "-D__TIME__=",
         "-D__TIMESTAMP__=",
         "-funwind-tables",
         "-fPIC",
         "-pipe",
         "-B../../third_party/binutils/Linux_x64/Release/bin",
         "-pthread",
         "-fcolor-diagnostics",
         "-m64",
         "-march=x86-64",
         "-Wall",
         "-Werror",
         "-Wextra",
         "-Wno-missing-field-initializers",
         "-Wno-unused-parameter",
         "-Wno-c++11-narrowing",
         "-Wno-covered-switch-default",
         "-Wno-unneeded-internal-declaration",
         "-Wno-inconsistent-missing-override",
         "-Wno-undefined-var-template",
         "-Wno-nonportable-include-path",
         "-Wno-address-of-packed-member",
         "-Wno-unused-lambda-capture",
         "-Wno-user-defined-warnings",
         "-Wno-enum-compare-switch",
         "-O2",
         "-fno-ident",
         "-fdata-sections",
         "-ffunction-sections",
         "-fno-omit-frame-pointer",
         "-g0",
         "-fvisibility=hidden",
         "-Xclang",
         "-load",
         "-Xclang",
         "../../third_party/llvm-build/Release+Asserts/lib/"
         "libFindBadConstructs.so",
         "-Xclang",
         "-add-plugin",
         "-Xclang",
         "find-bad-constructs",
         "-Xclang",
         "-plugin-arg-find-bad-constructs",
         "-Xclang",
         "check-auto-raw-pointer",
         "-Xclang",
         "-plugin-arg-find-bad-constructs",
         "-Xclang",
         "check-ipc",
         "-Wheader-hygiene",
         "-Wstring-conversion",
         "-Wtautological-overlap-compare",
         "-Wexit-time-destructors",
         "-Wno-header-guard",
         "-Wno-exit-time-destructors",
         "-std=gnu++14",
         "-fno-rtti",
         "-nostdinc++",
         "-isystem../../buildtools/third_party/libc++/trunk/include",
         "-isystem../../buildtools/third_party/libc++abi/trunk/include",
         "--sysroot=../../build/linux/debian_jessie_amd64-sysroot",
         "-fno-exceptions",
         "-fvisibility-inlines-hidden",
         "../../apps/app_lifetime_monitor.cc"},

        /* expected */
        {"../../third_party/llvm-build/Release+Asserts/bin/clang++",
         "-working-directory",
         "/w/c/s/out/Release",
         "-xc++",
         "-DV8_DEPRECATION_WARNINGS",
         "-DDCHECK_ALWAYS_ON=1",
         "-DUSE_UDEV",
         "-DUSE_ASH=1",
         "-DUSE_AURA=1",
         "-DUSE_NSS_CERTS=1",
         "-DUSE_OZONE=1",
         "-DDISABLE_NACL",
         "-DFULL_SAFE_BROWSING",
         "-DSAFE_BROWSING_CSD",
         "-DSAFE_BROWSING_DB_LOCAL",
         "-DCHROMIUM_BUILD",
         "-DFIELDTRIAL_TESTING_ENABLED",
         "-DCR_CLANG_REVISION=\"310694-1\"",
         "-D_FILE_OFFSET_BITS=64",
         "-D_LARGEFILE_SOURCE",
         "-D_LARGEFILE64_SOURCE",
         "-D__STDC_CONSTANT_MACROS",
         "-D__STDC_FORMAT_MACROS",
         "-DCOMPONENT_BUILD",
         "-DOS_CHROMEOS",
         "-DNDEBUG",
         "-DNVALGRIND",
         "-DDYNAMIC_ANNOTATIONS_ENABLED=0",
         "-DGL_GLEXT_PROTOTYPES",
         "-DUSE_GLX",
         "-DUSE_EGL",
         "-DANGLE_ENABLE_RELEASE_ASSERTS",
         "-DTOOLKIT_VIEWS=1",
         "-DV8_USE_EXTERNAL_STARTUP_DATA",
         "-DU_USING_ICU_NAMESPACE=0",
         "-DU_ENABLE_DYLOAD=0",
         "-DICU_UTIL_DATA_IMPL=ICU_UTIL_DATA_FILE",
         "-DUCHAR_TYPE=uint16_t",
         "-DGOOGLE_PROTOBUF_NO_RTTI",
         "-DGOOGLE_PROTOBUF_NO_STATIC_INITIALIZER",
         "-DHAVE_PTHREAD",
         "-DPROTOBUF_USE_DLLS",
         "-DSK_IGNORE_LINEONLY_AA_CONVEX_PATH_OPTS",
         "-DSK_HAS_PNG_LIBRARY",
         "-DSK_HAS_WEBP_LIBRARY",
         "-DSK_HAS_JPEG_LIBRARY",
         "-DSKIA_DLL",
         "-DGR_GL_IGNORE_ES3_MSAA=0",
         "-DSK_SUPPORT_GPU=1",
         "-DMESA_EGL_NO_X11_HEADERS",
         "-DBORINGSSL_SHARED_LIBRARY",
         "-DUSING_V8_SHARED",
         "-I../..",
         "-Igen",
         "-I../../third_party/libwebp/src",
         "-I../../third_party/khronos",
         "-I../../gpu",
         "-I../../third_party/ced/src",
         "-I../../third_party/icu/source/common",
         "-I../../third_party/icu/source/i18n",
         "-I../../third_party/protobuf/src",
         "-I../../skia/config",
         "-I../../skia/ext",
         "-I../../third_party/skia/include/c",
         "-I../../third_party/skia/include/config",
         "-I../../third_party/skia/include/core",
         "-I../../third_party/skia/include/effects",
         "-I../../third_party/skia/include/encode",
         "-I../../third_party/skia/include/gpu",
         "-I../../third_party/skia/include/images",
         "-I../../third_party/skia/include/lazy",
         "-I../../third_party/skia/include/pathops",
         "-I../../third_party/skia/include/pdf",
         "-I../../third_party/skia/include/pipe",
         "-I../../third_party/skia/include/ports",
         "-I../../third_party/skia/include/utils",
         "-I../../third_party/skia/third_party/vulkan",
         "-I../../third_party/skia/src/gpu",
         "-I../../third_party/skia/src/sksl",
         "-I../../third_party/mesa/src/include",
         "-I../../third_party/libwebm/source",
         "-I../../third_party/protobuf/src",
         "-Igen/protoc_out",
         "-I../../third_party/boringssl/src/include",
         "-I../../build/linux/debian_jessie_amd64-sysroot/"
         "usr/include/nss",
         "-I../../build/linux/debian_jessie_amd64-sysroot/"
         "usr/include/nspr",
         "-Igen",
         "-I../../third_party/WebKit",
         "-Igen/third_party/WebKit",
         "-I../../v8/include",
         "-Igen/v8/include",
         "-Igen",
         "-I../../third_party/flatbuffers/src/include",
         "-Igen",
         "-fno-strict-aliasing",
         "-Wno-builtin-macro-redefined",
         "-D__DATE__=",
         "-D__TIME__=",
         "-D__TIMESTAMP__=",
         "-funwind-tables",
         "-fPIC",
         "-pipe",
         "-B../../third_party/binutils/Linux_x64/Release/bin",
         "-pthread",
         "-fcolor-diagnostics",
         "-m64",
         "-march=x86-64",
         "-Wall",
         "-Werror",
         "-Wextra",
         "-Wno-missing-field-initializers",
         "-Wno-unused-parameter",
         "-Wno-c++11-narrowing",
         "-Wno-covered-switch-default",
         "-Wno-unneeded-internal-declaration",
         "-Wno-inconsistent-missing-override",
         "-Wno-undefined-var-template",
         "-Wno-nonportable-include-path",
         "-Wno-address-of-packed-member",
         "-Wno-unused-lambda-capture",
         "-Wno-user-defined-warnings",
         "-Wno-enum-compare-switch",
         "-O2",
         "-fno-ident",
         "-fdata-sections",
         "-ffunction-sections",
         "-fno-omit-frame-pointer",
         "-g0",
         "-fvisibility=hidden",
         "-Wheader-hygiene",
         "-Wstring-conversion",
         "-Wtautological-overlap-compare",
         "-Wexit-time-destructors",
         "-Wno-header-guard",
         "-Wno-exit-time-destructors",
         "-std=gnu++14",
         "-fno-rtti",
         "-nostdinc++",
         "-isystem../../buildtools/third_party/libc++/"
         "trunk/"
         "include",
         "-isystem../../buildtools/third_party/libc++abi/"
         "trunk/"
         "include",
         "--sysroot=&/w/c/s/out/Release/../../build/linux/"
         "debian_jessie_amd64-sysroot",
         "-fno-exceptions",
         "-fvisibility-inlines-hidden",
         "../../apps/app_lifetime_monitor.cc",
         "-resource-dir=/w/resource_dir/",
         "-Wno-unknown-warning-option",
         "-fparse-all-comments"});
  }

  TEST_CASE("Directory extraction") {
    ProjectConfig config;
    config.project_dir = "/w/c/s/";

    CompileCommandsEntry entry;
    entry.directory = "/base";
    entry.args = {"clang",
                  "-I/a_absolute1",
                  "--foobar",
                  "-I",
                  "/a_absolute2",
                  "--foobar",
                  "-Ia_relative1",
                  "--foobar",
                  "-I",
                  "a_relative2",
                  "--foobar",
                  "-iquote/q_absolute1",
                  "--foobar",
                  "-iquote",
                  "/q_absolute2",
                  "--foobar",
                  "-iquoteq_relative1",
                  "--foobar",
                  "-iquote",
                  "q_relative2",
                  "--foobar",
                  "foo.cc"};
    entry.file = "foo.cc";
    Project::Entry result =
        GetCompilationEntryFromCompileCommandEntry(&config, entry);

    std::unordered_set<std::string> angle_expected{
        "&/a_absolute1", "&/a_absolute2", "&/base/a_relative1",
        "&/base/a_relative2"};
    std::unordered_set<std::string> quote_expected{
        "&/q_absolute1", "&/q_absolute2", "&/base/q_relative1",
        "&/base/q_relative2"};
    REQUIRE(config.angle_dirs == angle_expected);
    REQUIRE(config.quote_dirs == quote_expected);
  }

  TEST_CASE("Entry inference") {
    Project p;
    {
      Project::Entry e;
      e.args = {"arg1"};
      e.filename = "/a/b/c/d/bar.cc";
      p.entries.push_back(e);
    }
    {
      Project::Entry e;
      e.args = {"arg2"};
      e.filename = "/a/b/c/baz.cc";
      p.entries.push_back(e);
    }

    // Guess at same directory level, when there are parent directories.
    {
      optional<Project::Entry> entry =
          p.FindCompilationEntryForFile("/a/b/c/d/new.cc");
      REQUIRE(entry.has_value());
      REQUIRE(entry->args == std::vector<std::string>{"arg1"});
    }

    // Guess at same directory level, when there are child directories.
    {
      optional<Project::Entry> entry =
          p.FindCompilationEntryForFile("/a/b/c/new.cc");
      REQUIRE(entry.has_value());
      REQUIRE(entry->args == std::vector<std::string>{"arg2"});
    }

    // Guess at new directory (use the closest parent directory).
    {
      optional<Project::Entry> entry =
          p.FindCompilationEntryForFile("/a/b/c/new/new.cc");
      REQUIRE(entry.has_value());
      REQUIRE(entry->args == std::vector<std::string>{"arg2"});
    }
  }

  TEST_CASE("Entry inference prefers same file endings") {
    Project p;
    {
      Project::Entry e;
      e.args = {"arg1"};
      e.filename = "common/simple_browsertest.cc";
      p.entries.push_back(e);
    }
    {
      Project::Entry e;
      e.args = {"arg2"};
      e.filename = "common/simple_unittest.cc";
      p.entries.push_back(e);
    }
    {
      Project::Entry e;
      e.args = {"arg3"};
      e.filename = "common/a/simple_unittest.cc";
      p.entries.push_back(e);
    }

    // Prefer files with the same ending.
    {
      optional<Project::Entry> entry =
          p.FindCompilationEntryForFile("my_browsertest.cc");
      REQUIRE(entry.has_value());
      REQUIRE(entry->args == std::vector<std::string>{"arg1"});
    }
    {
      optional<Project::Entry> entry =
          p.FindCompilationEntryForFile("my_unittest.cc");
      REQUIRE(entry.has_value());
      REQUIRE(entry->args == std::vector<std::string>{"arg2"});
    }
    {
      optional<Project::Entry> entry =
          p.FindCompilationEntryForFile("common/my_browsertest.cc");
      REQUIRE(entry.has_value());
      REQUIRE(entry->args == std::vector<std::string>{"arg1"});
    }
    {
      optional<Project::Entry> entry =
          p.FindCompilationEntryForFile("common/my_unittest.cc");
      REQUIRE(entry.has_value());
      REQUIRE(entry->args == std::vector<std::string>{"arg2"});
    }

    // Prefer the same directory over matching file-ending.
    {
      optional<Project::Entry> entry =
          p.FindCompilationEntryForFile("common/a/foo.cc");
      REQUIRE(entry.has_value());
      REQUIRE(entry->args == std::vector<std::string>{"arg3"});
    }
  }
}
