// test_scripts_storage.cpp — scriptNameIsValid, the one host-testable
// piece of scripts_storage.cpp (mount/read/save need real LittleFS
// hardware; see the file's header comment).

#include "scripts_storage.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failCount = 0;

#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++;                                          \
    }                                                          \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

static bool valid(const char* s) { return scriptNameIsValid(s, std::strlen(s)); }

void test_ordinary_names_are_valid() {
  TEST("ordinary script names are valid");
  CHECK(valid("boot.fnl"));
  CHECK(valid("verse-wash.fnl"));
  CHECK(valid("a"));
  CHECK(valid("my_effect.lua"));
}

void test_rejects_empty_and_null() {
  TEST("rejects empty name and null pointer");
  CHECK(!scriptNameIsValid("", 0));
  CHECK(!scriptNameIsValid(nullptr, 0));
}

void test_rejects_path_separator() {
  TEST("rejects any name containing '/'");
  CHECK(!valid("sub/dir.fnl"));
  CHECK(!valid("/etc/passwd"));
  CHECK(!valid("a/"));
}

void test_rejects_path_traversal_attempts() {
  TEST("rejects path-traversal attempts (a script_save/load/delete name is "
       "a single flat path component, never a path)");
  CHECK(!valid("../evil"));
  CHECK(!valid("../../show"));      // would otherwise land on the "show" partition's data
  CHECK(!valid("../../../etc/passwd"));
  CHECK(!valid("a/../../b"));
}

void test_rejects_leading_dot() {
  TEST("rejects any leading dot, not just exactly '.' and '..'");
  CHECK(!valid("."));
  CHECK(!valid(".."));
  CHECK(!valid("..fnl"));       // starts with dots but isn't exactly ".."
  CHECK(!valid(".hidden.fnl"));  // would otherwise risk colliding with
                                 // scripts_storage_save's ".<name>.tmp"
                                 // staging file -- see its own comment
}

void test_rejects_embedded_nul() {
  TEST("rejects a name with an embedded NUL byte");
  char withNul[] = {'a', '\0', 'b'};
  CHECK(!scriptNameIsValid(withNul, sizeof(withNul)));
}

void test_rejects_characters_outside_the_whitelist() {
  TEST("rejects whitespace, control characters, and other punctuation "
       "outside the [A-Za-z0-9_.-] whitelist");
  CHECK(!valid("has space.fnl"));
  CHECK(!valid("semi;colon.fnl"));
  CHECK(!valid("back\\slash.fnl"));
  char withControlChar[] = {'a', '\x01', 'b'};
  CHECK(!scriptNameIsValid(withControlChar, sizeof(withControlChar)));
}

void test_rejects_too_long() {
  TEST("rejects a name longer than LittleFS's filename limit");
  std::string longName(300, 'x');
  CHECK(!scriptNameIsValid(longName.data(), longName.size()));
  std::string okName(255, 'x');
  CHECK(scriptNameIsValid(okName.data(), okName.size()));
}

void test_boot_filename_itself_is_valid() {
  TEST("saving as SCRIPTS_BOOT_FILENAME is allowed (that's how boot.fnl is set)");
  CHECK(valid(SCRIPTS_BOOT_FILENAME));
}

int main() {
  test_ordinary_names_are_valid();
  test_rejects_empty_and_null();
  test_rejects_path_separator();
  test_rejects_path_traversal_attempts();
  test_rejects_leading_dot();
  test_rejects_embedded_nul();
  test_rejects_characters_outside_the_whitelist();
  test_rejects_too_long();
  test_boot_filename_itself_is_valid();

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  }
  printf("\n%d test(s) failed.\n", g_failCount);
  return 1;
}
