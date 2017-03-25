#include "test.h"

#include "indexer.h"
#include "platform.h"
#include "serializer.h"
#include "utils.h"

void Write(const std::vector<std::string>& strs) {
  for (const std::string& str : strs)
    std::cout << str << std::endl;
}

std::string ToString(const rapidjson::Document& document) {
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  writer.SetFormatOptions(
    rapidjson::PrettyFormatOptions::kFormatSingleLineArray);
  writer.SetIndent(' ', 2);

  buffer.Clear();
  document.Accept(writer);
  return buffer.GetString();
}

std::vector<std::string> split_string(const std::string& str, const std::string& delimiter) {
  // http://stackoverflow.com/a/13172514
  std::vector<std::string> strings;

  std::string::size_type pos = 0;
  std::string::size_type prev = 0;
  while ((pos = str.find(delimiter, prev)) != std::string::npos) {
    strings.push_back(str.substr(prev, pos - prev));
    prev = pos + 1;
  }

  // To get the last substring (or only, if delimiter is not found)
  strings.push_back(str.substr(prev));

  return strings;
}


void DiffDocuments(rapidjson::Document& expected, rapidjson::Document& actual) {
  std::vector<std::string> actual_output;
  {
    std::string buffer = ToString(actual);
    actual_output = split_string(buffer, "\n");
  }

  std::vector<std::string> expected_output;
  {
    std::string buffer = ToString(expected);
    expected_output = split_string(buffer, "\n");
  }

  int len = std::min(actual_output.size(), expected_output.size());
  for (int i = 0; i < len; ++i) {
    if (actual_output[i] != expected_output[i]) {
      std::cout << "Line " << i << " differs:" << std::endl;
      std::cout << "  expected: " << expected_output[i] << std::endl;
      std::cout << "  actual:   " << actual_output[i] << std::endl;
    }
  }

  if (actual_output.size() > len) {
    std::cout << "Additional output in actual:" << std::endl;
    for (int i = len; i < actual_output.size(); ++i)
      std::cout << "  " << actual_output[i] << std::endl;
  }

  if (expected_output.size() > len) {
    std::cout << "Additional output in expected:" << std::endl;
    for (int i = len; i < expected_output.size(); ++i)
      std::cout << "  " << expected_output[i] << std::endl;
  }
}

void VerifySerializeToFrom(IndexedFile& file) {
  return; // TODO: reenable
  std::string expected = file.ToString();
  std::string actual = Deserialize("foo.cc", Serialize(file)).ToString();
  if (expected != actual) {
    std::cerr << "Serialization failure" << std::endl;;
    assert(false);
  }
}

void RunTests() {
  // TODO: Assert that we need to be on clang >= 3.9.1

  /*
  ParsingDatabase db = Parse("tests/vars/function_local.cc");
  std::cout << std::endl << "== Database ==" << std::endl;
  std::cout << db.ToString();
  std::cin.get();
  return 0;
  */

  for (std::string path : GetFilesInFolder("tests", true /*recursive*/, true /*add_folder_to_path*/)) {
    //if (path != "tests/templates/specialized_func_definition.cc") continue;
    //if (path != "tests/templates/namespace_template_class_template_func_usage_folded_into_one.cc") continue;
    if (path != "tests/foo2.cc") continue;
    //if (path != "tests/namespaces/namespace_reference.cc") continue;
    //if (path != "tests/templates/implicit_variable_instantiation.cc") continue;

    //if (path != "tests/templates/template_class_type_usage_folded_into_one.cc") continue;
    //path = "C:/Users/jacob/Desktop/superindex/indexer/" + path;

    // Parse expected output from the test, parse it into JSON document.
    std::string expected_output;
    ParseTestExpectation(path, &expected_output);
    rapidjson::Document expected;
    expected.Parse(expected_output.c_str());

    // Run test.
    std::cout << "[START] " << path << std::endl;
    IndexedFile db = Parse(path, {"-IC:/Users/jacob/Desktop/superindex/src"}, false /*dump_ast*/);
    VerifySerializeToFrom(db);
    std::string actual_output = db.ToString();

    rapidjson::Document actual;
    actual.Parse(actual_output.c_str());

    if (actual == expected) {
      std::cout << "[PASSED] " << path << std::endl;
    }
    else {
      std::cout << "[FAILED] " << path << std::endl;
      std::cout << "Expected output for " << path << ":" << std::endl;
      std::cout << expected_output;
      std::cout << "Actual output for " << path << ":" << std::endl;
      std::cout << actual_output;
      std::cout << std::endl;
      std::cout << std::endl;
      DiffDocuments(expected, actual);
      break;
    }
  }

  std::cin.get();
}

// TODO: ctor/dtor, copy ctor