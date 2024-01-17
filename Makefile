CXX = g++
CXXFLAGS = --std=c++17 -msse4.1 -mavx2 -O3
TARGET = wcl
SRC = wcl.cpp

.PHONY: all clean

all: $(TARGET)

avx:
	$(CXX) $(CXXFLAGS) -o $(TARGET)_avx wcl_avx.cpp


$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

