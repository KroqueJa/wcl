#include <iostream>
#include <fstream>
#include <cstdlib>
#include <immintrin.h>
#include <stdint.h>
#include <sys/stat.h>
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <algorithm>
#include <execution>



// Struct to keep file data ptr and file size together
struct FileData {
  size_t fileSize;
  char* contents;
};

// Struct to keep the results clean
struct Result {
  std::string str;
  size_t lineCount;
};


// Global threading things
std::mutex fileSetMutex;
std::mutex outputMutex;
std::unordered_set<std::string> fileSet;
std::vector<Result> output;

size_t processBuffer(char* buf, size_t bufferSize) {
  __m128i vec_target = _mm_set1_epi8('\n');

  size_t sum = 0;
  char* tmp = buf;
  size_t bytesRead = 0;

  // Process in chunks of 16 bytes
  while (bytesRead < bufferSize && ((uintptr_t)tmp % 16 != 0)) {
    if (*tmp == '\n') ++sum;
    ++tmp;
    ++bytesRead;
  }

  __m128i chunk, result;
  int mask;
  while (bytesRead + 15 < bufferSize) {             // Process 16 bytes at a time
    chunk = _mm_loadu_si128((__m128i*)tmp);         // Load 16 bytes
    result = _mm_cmpeq_epi8(chunk, vec_target);     // Compare all 16 bytes with target
    mask = _mm_movemask_epi8(result);               // Get a mask of comparison results
    sum += _mm_popcnt_u32(mask);                    // Count the number of 1 bits in the mask
    tmp += 16;
    bytesRead += 16;
  }

  // Process remaining characters
  while (bytesRead < bufferSize) {

    if (*tmp == '\n') ++sum;
    ++tmp;
    ++bytesRead;
  }

  return sum;
}

FileData readFileIntoBuffer(const std::string& filename) {
  struct stat st;
  if (stat(filename.c_str(), &st) != 0) {
    std::cerr << "Error getting file size: " << filename << '\n';
    return {0, nullptr}; // Return empty FileData on error
  }

  size_t bufferSize = st.st_size;
  char* buffer = static_cast<char*>(malloc(bufferSize));
  if (buffer == nullptr) {
    std::cerr << "Memory allocation failed." << '\n';
    return {0, nullptr}; // Return empty FileData on memory allocation failure
  }

  std::ifstream file(filename, std::ios::binary);
  if (!file.is_open()) {
    std::cerr << "Error opening file: " << filename << '\n';
    free(buffer);
    return {0, nullptr}; // Return empty FileData on file open failure
  }

  file.read(buffer, bufferSize);
  file.close();

  return {bufferSize, buffer}; // Return filled FileData
}

void processFile() {
    while (true) {
        std::string filename;

        {
            std::lock_guard<std::mutex> guard(fileSetMutex);
            if (fileSet.empty()) {
                return; // No more files to process
            }
            filename = *fileSet.begin();
            fileSet.erase(fileSet.begin());
        }

        FileData fileData = readFileIntoBuffer(filename);
        if (fileData.contents) {
            size_t count = processBuffer(fileData.contents, fileData.fileSize);
            free(fileData.contents);

            std::lock_guard<std::mutex> outputGuard(outputMutex);
            output.push_back({std::to_string(count) + " " + filename, count});
        } else {
            std::cerr << "Could not process file " << filename << '\n';
            // Handle error appropriately
        }
    }
}

int main(int argc, char** argv) {
    // Populate fileSet with input filenames
    for (int i = 1; i < argc; ++i) {
        fileSet.insert(argv[i]);
    }

    unsigned numThreads = std::thread::hardware_concurrency();
    std::vector<std::thread> threads(numThreads);

    // Start threads
    for (unsigned i = 0; i < numThreads; ++i) {
        threads[i] = std::thread(processFile);
    }

    // Join threads
    for (auto& thread : threads) {
        thread.join();
    }

    size_t total = 0;

    // Print results
    for (const auto& result : output) {
        total += result.lineCount;
        std::cout << result.str << '\n';
    }

    std::cout << total << std::endl;

    return 0;
}
