#include <fcntl.h>
#include <immintrin.h>
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

// Mutexes
std::mutex fileSetMutex;
std::mutex outputMutex;

// Set to keep input arguments
std::unordered_set<std::string> fileSet;

// Vector in which to store results
std::vector<Result> output;

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
        exit( 1 );  // TODO: real error handling
      }
    }

    // Define a buffer for chunks of the file
    const size_t bufferSize = 163840; // Size of the chunk buffer
    char buffer[bufferSize];          // The buffer itself
    ssize_t bytesRead;                // Track how many bytes have been read

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
        result1 = _mm256_cmpeq_epi8(
            chunk1, vec_target
        );

        // Compare each byte in the second chunk
        result2 = _mm256_cmpeq_epi8(
            chunk2, vec_target
        );
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

int main( int argc, char** argv )
{
  if ( argc == 1 ) {
    // No arguments provided, read from STDIN

    // An empty string is used as a placeholder to signal reading from STDIN
    fileSet.insert( "" );
    processFile();
    std::cout << output[0].lineCount << std::endl;
  } else {
    // Populate fileSet with input filenames
    for ( int i = 1; i < argc; ++i ) fileSet.insert( argv[i] );
    unsigned numThreads = std::thread::hardware_concurrency();
    std::vector<std::thread> threads( numThreads );

    // Start threads
    for ( unsigned i = 0; i < numThreads; ++i )
      threads[i] = std::thread( processFile );

    // Join threads
    for ( auto& thread: threads ) thread.join();

    size_t total = 0;

    // Print results
    for ( const auto& result: output ) {
      total += result.lineCount;
      if ( argc > 2 ) std::cout << result.str << '\n';
    }

    std::cout << total << std::endl;
  }

  return 0;
}
