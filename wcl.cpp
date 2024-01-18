#include <fcntl.h>
#include <immintrin.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

// Struct to keep the results clean
struct Result
{
  std::string str;
  size_t lineCount;
};

// Flags
bool countBytes = false;

// Mutexes
std::mutex fileSetMutex;
std::mutex outputMutex;

// Set to keep input arguments
std::unordered_set<std::string> fileSet;

// Vector in which to store results
std::vector<Result> output;

void processFileSize()
{
  while ( true ) {
    // ---- Get a file name from the unordered set ----
    std::string filename;

    {
      std::lock_guard<std::mutex> guard( fileSetMutex );
      if ( fileSet.empty() ) return;
      filename = *fileSet.begin();
      fileSet.erase( fileSet.begin() );
    }

    // ---- Open a file (or read STDIN) ----
    int fd;
    if ( filename.empty() ) {  // If filename is empty, read from STDIN
      fd = 0;
    } else {
      // Get file descriptor
      fd = open( filename.c_str(), O_RDONLY );
      if ( fd < 0 ) {
        std::cerr << "Error opening file: " << filename << '\n';
        return;
      }
    }

    struct stat stat_buf;
    int rc = stat( filename.c_str(), &stat_buf );
    size_t fileSize = rc == 0 ? stat_buf.st_size : 0;

    {
      std::lock_guard<std::mutex> outputGuard( outputMutex );
      output.push_back( { std::to_string( fileSize ) + " " + filename,
                          fileSize } );
    }
  }
}

// Function to process a file path and put a `Result` struct into the `output`
// vector
void processFile()
{
  while ( true ) {
    // ---- Get a file name from the unordered set ----
    std::string filename;

    {
      std::lock_guard<std::mutex> guard( fileSetMutex );
      if ( fileSet.empty() ) return;
      filename = *fileSet.begin();
      fileSet.erase( fileSet.begin() );
    }

    // ---- Open a file (or read STDIN) ----
    int fd;
    if ( filename.empty() ) {  // If filename is empty, read from STDIN
      fd = 0;
    } else {
      // Get file descriptor
      fd = open( filename.c_str(), O_RDONLY );
      if ( fd < 0 ) {
        std::cerr << "Error opening file: " << filename << '\n';
        return;
      }
    }

    // Define a buffer for chunks of the file
    const size_t bufferSize = 163840;  // Size of the chunk buffer
    char buffer[bufferSize];           // The buffer itself
    ssize_t bytesRead;                 // Track how many bytes have been read

    __m256i chunk1, chunk2, result1, result2;
    int mask1, mask2;  // Bitmasks to hold the comparison results

    // ---- Read the file into the buffer, and count newlines in each chunk ----
    size_t totalLines = 0;
    while ( ( bytesRead = read( fd, buffer, bufferSize ) ) > 0 ) {
      __m256i vec_target = _mm256_set1_epi8( '\n' );  // Vector of newlines
      size_t lines = 0;           // Newlines in current chunk
      char* tmp = buffer;         // Temporary pointer to the buffer
      size_t processedBytes = 0;  // How many bytes we've read so far

      // Align to 32-byte boundary to optimize vector instructions
      while ( processedBytes < bytesRead && ( (uintptr_t)tmp % 32 != 0 ) ) {
        if ( *tmp == '\n' ) ++lines;
        ++tmp;
        ++processedBytes;
      }

      // Process buffer in 64-byte chunks
      while ( processedBytes + 63 < bytesRead ) {
        // Load bytes into chunks
        chunk1 = _mm256_loadu_si256( (__m256i*)tmp );
        chunk2 = _mm256_loadu_si256( (__m256i*)( tmp + 32 ) );

        // Compare each byte in the first chunk
        result1 = _mm256_cmpeq_epi8( chunk1, vec_target );

        // Compare each byte in the second chunk
        result2 = _mm256_cmpeq_epi8( chunk2, vec_target );
        // Create masks for chunks
        mask1 = _mm256_movemask_epi8( result1 );
        mask2 = _mm256_movemask_epi8( result2 );
        // Count newlines in both chunks
        lines += _mm_popcnt_u32( mask1 ) + _mm_popcnt_u32( mask2 );

        // Move to the next chunk
        tmp += 64;
        processedBytes += 64;
      }

      // Process remaining characters
      while ( processedBytes < bytesRead ) {
        if ( *tmp == '\n' ) ++lines;
        ++tmp;
        ++processedBytes;
      }

      totalLines += lines;
    }
    // Only close the file if we opened it
    if ( !filename.empty() ) close( fd );

    // Push result onto output vector
    {
      std::lock_guard<std::mutex> outputGuard( outputMutex );
      output.push_back( { std::to_string( totalLines ) + " " + filename,
                          totalLines } );
    }
  }
}
void processStdinBytes()
{
  size_t totalBytes = 0;
  const size_t bufferSize = 16384;
  char buffer[bufferSize];

  ssize_t bytesRead;
  while ( ( bytesRead = read( 0, buffer, bufferSize ) ) > 0 ) {
    totalBytes += bytesRead;
  }

  std::cout << totalBytes << std::endl;
}

int main( int argc, char** argv )
{
  for ( int i = 1; i < argc; ++i ) {
    if ( std::string( argv[i] ) == "-b" ) {
      countBytes = true;
      continue;
    }
    fileSet.insert( argv[i] );
  }

  if ( argc == 1 || ( argc == 2 && countBytes ) ) {
    // No arguments provided or only '-b', read from STDIN
    if ( countBytes ) {
      // Call a function to read from STDIN and count bytes
      processStdinBytes();
    } else {
      // Call the existing function to count lines from STDIN
      fileSet.insert( "" );
      processFile();
    }
  } else {
    unsigned numThreads = std::thread::hardware_concurrency();
    std::vector<std::thread> threads( numThreads );

    // Select function based on countBytes flag
    auto processFunction = countBytes ? processFileSize : processFile;

    // Start threads
    for ( unsigned i = 0; i < numThreads; ++i ) {
      threads[i] = std::thread( processFunction );
    }

    // Join threads
    for ( auto& thread: threads ) thread.join();

    // Print results
    size_t total = 0;
    for ( const auto& result: output ) {
      total += result.lineCount;
      if ( argc > 2 ) std::cout << result.str << '\n';
    }
    std::cout << total << std::endl;
  }

  return 0;
}
