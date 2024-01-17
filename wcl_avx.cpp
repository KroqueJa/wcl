#include <iostream>
#include <fstream>
#include <immintrin.h>
#include <stdint.h>
#include <sys/stat.h>
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <fcntl.h>
#include <unistd.h>

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

// TODO: investigate parallel accumulators
// TODO: Process 64 bytes at a time
// TODO: Horizontal summation with _mm256_sad_epu8
// TODO: Process file descriptor directly
// TODO: Investigate allocating large buffers (one per thread) directly instead of dynamically checking file sizes

void processFile() {
    while (true) {
        std::string filename;

        {
            std::lock_guard<std::mutex> guard(fileSetMutex);
            if (fileSet.empty()) return;
            filename = *fileSet.begin();
            fileSet.erase(fileSet.begin());
        }

        int fd = open(filename.c_str(), O_RDONLY);
        if (fd < 0) {
            std::cerr << "Error opening file: " << filename << '\n';
            exit(1); // TODO: real error handling
        }

        const size_t bufferSize = 163840;
        char buffer[bufferSize];
        ssize_t bytesRead;

        size_t totalLines = 0;
        while ((bytesRead = read(fd, buffer, bufferSize)) > 0) {
            __m256i vec_target = _mm256_set1_epi8('\n');
            size_t lines = 0;
            char* tmp = buffer;
            size_t processedBytes = 0;

            // Process buffer
            // Align to 32-byte boundary
            while (processedBytes < bytesRead && ((uintptr_t)tmp % 32 != 0)) {
                if (*tmp == '\n') ++lines;
                ++tmp;
                ++processedBytes;
            }

            __m256i chunk, result;
            int mask;
            while (processedBytes + 31 < bytesRead) { // Process 32 bytes at a time
                chunk = _mm256_loadu_si256((__m256i*)tmp);
                result = _mm256_cmpeq_epi8(chunk, vec_target);
                mask = _mm256_movemask_epi8(result);
                lines += _mm_popcnt_u32(mask);
                tmp += 32;
                processedBytes += 32;
            }

            // Process remaining characters
            while (processedBytes < bytesRead) {
                if (*tmp == '\n') ++lines;
                ++tmp;
                ++processedBytes;
            }

            totalLines += lines;
        }

        close(fd);

        {
            std::lock_guard<std::mutex> outputGuard(outputMutex);
            output.push_back({std::to_string(totalLines) + " " + filename, totalLines});
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
