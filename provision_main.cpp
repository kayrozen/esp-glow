#include "provision.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static std::string readFileFromDisk(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return "";
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

int main(int argc, char* argv[]) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <show.show> <output.shw1>\n", argv[0]);
    return 1;
  }

  const char* showPath = argv[1];
  const char* outputPath = argv[2];

  // Read the .show file
  std::string showText = readFileFromDisk(showPath);
  if (showText.empty()) {
    fprintf(stderr, "Error: cannot read %s\n", showPath);
    return 1;
  }

  // Compile the show
  CompileResult result = compileShow(showText, readFileFromDisk);

  if (!result.ok) {
    fprintf(stderr, "Compilation error: %s\n", result.err.c_str());
    return 1;
  }

  // Write the bundle
  std::ofstream outFile(outputPath, std::ios::binary);
  if (!outFile.is_open()) {
    fprintf(stderr, "Error: cannot write %s\n", outputPath);
    return 1;
  }

  outFile.write(reinterpret_cast<const char*>(result.bundle.data()), result.bundle.size());
  outFile.close();

  printf("Successfully compiled %s -> %s (%zu bytes)\n", showPath, outputPath, result.bundle.size());
  return 0;
}
